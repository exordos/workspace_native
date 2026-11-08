#include "native-controller.h"
#include "native-chat-key-vault.h"
#include "native-chat-crypto.h"
#include "realtime-client.h"
#include "mock-server.h"
#ifdef WORKSPACE_NATIVE_E2EE_UI
#include "native-app.h"
#include "native-markdown.h"
#include "native-preferences.h"
#include <QApplication>
#include <QImage>
#include <QCryptographicHash>
#include <QAccessible>
#include <QClipboard>
#include <QQmlApplicationEngine>
#include <QQmlProperty>
#include <QQmlExpression>
#include <QJSValue>
#include <QDateTime>
#include <QColor>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#endif

#include <QFile>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QUuid>
#include <QtEndian>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>

using namespace workspace;
using namespace workspace::test;
namespace {
const QString Issuer = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const QString IssuerDevice = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
const QString OtherGeneration = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
QString CertificatePath, PrivateKeyPath;
class Secrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    void read(const QString &key, Loaded done) override {
        const auto value = values.value(key);
        QTimer::singleShot(0, this, [done, value] { done(value, value.isEmpty() ? Error::NotFound : Error::None); });
    }
    void write(const QString &key, const QByteArray &value, Completion done) override {
        values.insert(key, value); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void remove(const QString &key, Completion done) override { values.remove(key); done(Error::None); }
};

// Real TLS: production ApiClient and Vault retain their HTTPS ownership policy.
// The process trusts only this fixture's synthetic CA in addition to its defaults.
class TlsGateway final : public QTcpServer {
public:
    QSslCertificate certificate;
    QSslKey key;
    quint16 upstream;
    explicit TlsGateway(quint16 port) : upstream(port) {
        QFile cert(CertificatePath);
        QFile secret(PrivateKeyPath);
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

QJsonObject encryptedStream() {
    return {{"uuid", streamId}, {"project_id", projectId}, {"name", "Encrypted fixture"},
        {"user_uuid", userId}, {"owner", Issuer}, {"role", "member"}, {"source_name", "native"},
        {"provider_uuid", QJsonValue::Null}, {"provider_identity", QJsonValue::Null},
        {"encryption", true}, {"current_encryption_key_uuid", returnedId}, {"default_topic_uuid", secondTopicId}};
}
// Complete source DTO: archived admission intentionally requires more than the
// sparse legacy active-stream fixture. It is metadata, never key material.
QJsonObject fullEncryptedStream(bool archived = false) {
    auto row = encryptedStream();
    row["description"] = "Synthetic encrypted Archive fixture";
    row["private"] = false; row["direct_user_uuid"] = QJsonValue::Null;
    row["invite_only"] = false; row["announce"] = false; row["is_archived"] = archived;
    row["unread_count"] = 0; row["active_unread_count"] = 0; row["passive_unread_count"] = 0;
    row["last_message_uuid"] = QJsonValue::Null; row["notification_mode"] = "all_messages";
    row["created_at"] = "2026-01-02T10:00:00.000Z";
    row["updated_at"] = "2026-01-02T10:00:01.000Z";
    return row;
}
QJsonObject cryptoEvent(const QJsonObject &message) {
    auto payload = message; payload["kind"] = "message.created";
    return {{"uuid", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"project_id", projectId},
        {"user_uuid", userId}, {"payload", payload}};
}
QJsonObject selfBindingEvent(const QString &kind) {
    auto result = cryptoEvent(QJsonObject{{"uuid", messageId}, {"project_id", projectId}, {"stream_uuid", streamId},
        {"user_uuid", userId}, {"who_uuid", Issuer}, {"role", "member"}, {"notification_mode", "all_messages"},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:01:00Z"}});
    auto payload = result.value("payload").toObject(); payload["kind"] = kind; result["payload"] = payload;
    return result;
}
QVariantMap displayed(const NativeController &controller, const QString &id) {
    for (const auto &row : controller.messages()) if (row.toMap().value("id").toString() == id) return row.toMap();
    return {};
}

class Fixture {
public:
    MockServer http;
    TlsGateway tls{http.serverPort()};
    ApiClient api;
    Session session{&api};
    NativeController controller{&api, &session};
    QTemporaryDir directory;
    Secrets secrets;
    NativeChatKeyVault vault{directory.path(), &secrets};
    QJsonObject requestBody, savedRequest;
    QVector<QJsonObject> requestBodies;
    QMap<QString, QJsonObject> savedRequests;
    QJsonObject stream = encryptedStream();
    QString issuerRole = "owner";
    QJsonObject cipher = message({}, messageId);
    int posts = 0, streamGets = 0, bindingGets = 0, topicGets = 0, postDelay = 0, streamDelay = 0;
    int catalogGets = 0, catalogDelay = 0, streamStatus = 200, checkDelay = 0;
    QString deviceName = "Synthetic native TLS device";
    bool legacyCatalog = false, catalogOwnsStream = true;
    Fixture() {
        cipher["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId},
            {"content", QString::fromLatin1(QByteArray(48, 'x').toBase64())}};
        cipher["author_uuid"] = Issuer; cipher["is_own"] = false;
        http.handler = [this](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/") {
                ++catalogGets;
                const QJsonArray rows = catalogOwnsStream ? QJsonArray{legacyCatalog
                    ? QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Legacy encrypted fixture"}, {"encryption", true}}
                    : stream} : QJsonArray{};
                return {rows, 200, catalogDelay};
            }
            if (path == "/api/workspace/v1/messenger/streams/" + streamId) { ++streamGets; return {stream, streamStatus, streamDelay}; }
            if (path == "/api/workspace/v1/users/") return {QJsonArray{}};
            if (path == "/api/workspace/v1/messenger/stream_topics/") {
                ++topicGets;
                return {QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                    {"name", "Visible history"}, {"is_default", stream.value("default_topic_uuid").toString() == topicId}},
                    QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                    {"name", "Actual default"}, {"is_default", stream.value("default_topic_uuid").toString() == secondTopicId}}}};
            }
            if (path == "/api/workspace/v1/messenger/stream_bindings/") {
                ++bindingGets;
                return {QJsonArray{QJsonObject{{"uuid", messageId}, {"project_id", projectId}, {"stream_uuid", streamId},
                    {"user_uuid", userId}, {"role", "member"}}, QJsonObject{{"uuid", returnedId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"user_uuid", Issuer}, {"role", issuerRole}}}};
            }
            if (path == "/api/workspace/v1/messenger/messages/" && request.method == "POST") {
                ++posts; requestBody = request.body; requestBodies.append(request.body);
                const auto publicId = QUuid::createUuidV5(QUuid(request.body.value("topic_uuid").toString()), request.body.value("uuid").toString().toUtf8())
                    .toString(QUuid::WithoutBraces);
                savedRequest = message({}, publicId, request.body.value("topic_uuid").toString()); savedRequest["payload"] = request.body.value("payload");
                savedRequests.insert(publicId, savedRequest);
                return {savedRequest, 201, postDelay};
            }
            if (path.startsWith("/api/workspace/v1/messenger/messages/") && !path.endsWith('/')) {
                const auto id = path.section('/', -1);
                return savedRequests.contains(id) ? Response{savedRequests.value(id), 200, checkDelay} : Response{QJsonObject{}, 404, checkDelay};
            }
            if (path == "/api/workspace/v1/messenger/messages/") return {QJsonArray{cipher}};
            return sessionResponse(request);
        };
    }
    bool start(bool archived = false) {
        const auto failedStage = [this](const char *stage) {
            QJsonArray pending;
            for (auto *reply : api.findChildren<QNetworkReply *>())
                pending.append(QJsonObject{{"path", reply->url().path()}, {"finished", reply->isFinished()},
                    {"method", QString::fromLatin1(reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray())}});
            qInfo().noquote() << "FIXTURE_START_DIAGNOSTIC" << stage
                << QJsonDocument(QJsonObject{{"projects", controller.projects().size()}, {"streams", controller.streams().size()},
                    {"topics", controller.topics().size()}, {"messages", controller.messages().size()}, {"busy", controller.busy()},
                    {"streamGets", streamGets}, {"bindingGets", bindingGets}, {"topicGets", topicGets},
                    {"catalogGets", catalogGets}, {"pending", pending}}).toJson(QJsonDocument::Compact);
            return false;
        };
        controller.login(tls.origin().toString(), "Synthetic", "synthetic-password", "");
        if (!QTest::qWaitFor([this] { return controller.projects().size() == 1; }, 2000)) return failedStage("projects");
        controller.openProject(projectId);
        if (!QTest::qWaitFor([this, archived] { return archived ? controller.archivedChats().size() == 1
            : controller.streams().size() == 1; }, 2000)) return failedStage("streams");
        if (archived) controller.openArchivedChat(streamId);
        else controller.selectStream(streamId);
        if (!QTest::qWaitFor([this] { return controller.topics().size() == 2; }, 2000)) return failedStage("topics");
        if (!archived) controller.selectTopic(topicId);
        if (!QTest::qWaitFor([this] { return !controller.busy() && controller.messages().size() == 1; }, 2000)) return failedStage("history");
        bool opened = false;
        vault.open(tls.origin(), projectId, userId, deviceName,
            [&opened](NativeChatKeyVault::Failure failure) { opened = failure == NativeChatKeyVault::Failure::None; });
        if (!QTest::qWaitFor([&opened] { return opened; }, 2000)) return failedStage("vault");
        controller.useChatKeys(&vault);
        return true;
    }
    QJsonObject sourceResponse(bool reject = false) const { return sourceResponseFor(requestBody, savedRequest, reject); }
    QJsonObject sourceResponseFor(const QJsonObject &body, const QJsonObject &saved, bool reject = false) const {
        const auto payload = body.value("payload").toObject();
        QProcess peer;
        peer.start(QStringLiteral(WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE), {QStringLiteral(WORKSPACE_NATIVE_E2EE_PEER_SCRIPT)});
        if (!peer.waitForStarted()) return {};
        const QJsonObject input{{"project", projectId}, {"stream", streamId}, {"key", payload.value("key_uuid")},
            {"request", saved.value("uuid")}, {"requester", userId}, {"author", Issuer}, {"senderDevice", IssuerDevice},
            {"recipientDevice", payload.value("device_id")}, {"publicKey", payload.value("public_key")},
            {"role", "owner"}, {"plaintext", "Synthetic source-granted plaintext"}, {"reject", reject}};
        peer.write(QJsonDocument(input).toJson(QJsonDocument::Compact)); peer.closeWriteChannel();
        if (!peer.waitForFinished(10000) || peer.exitCode() != 0) return {};
        return QJsonDocument::fromJson(peer.readAllStandardOutput()).object();
    }
    QJsonObject responseDto(const QJsonObject &source) const {
        const auto result = source.value("message").toObject();
        auto dto = message({}, result.value("uuid").toString(), secondTopicId);
        dto["author_uuid"] = Issuer; dto["is_own"] = false; dto["payload"] = result.value("payload");
        return dto;
    }
    bool seedRequest(const QString &key, const QString &binding, bool originalTarget,
        NativeChatKeyVault::PendingRequest &pending, QJsonObject &body, QJsonObject &own, const QString &fixtureLocalId = {}) {
        auto pair = NativeChatCrypto::createRequestKeyPair(); if (!pair) return false;
        const auto cleanup = qScopeGuard([&] { pair.privateKey.fill(0); pair.privateKey.clear(); });
        pending = {fixtureLocalId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : fixtureLocalId, {}, vault.device().name,
            pair.publicKey, pair.privateKey, 1234, 0, {}, originalTarget ? secondTopicId : QString{}};
        const auto publicId = QUuid::createUuidV5(QUuid(secondTopicId), pending.localId.toUtf8()).toString(QUuid::WithoutBraces);
        if (binding == "public") pending.messageId = publicId;
        else if (binding == "legacy-local") pending.messageId = pending.localId;
        if (vault.putPendingRequest(vault.lease(), {streamId, key}, pending) != NativeChatKeyVault::Failure::None) return false;
        const QJsonObject payload{{"kind", "key_request"}, {"key_uuid", key}, {"device_id", vault.device().id},
            {"device_name", pending.deviceName}, {"public_key", QString::fromLatin1(pending.publicKey.toBase64())}};
        own = message({}, publicId, secondTopicId); own["payload"] = payload;
        body = {{"uuid", pending.localId}, {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}, {"payload", payload}};
        return true;
    }
    bool prepareA() {
        controller.requestChatKey(returnedId);
        if (!QTest::qWaitFor([this] { return posts == 1 && !vault.pendingRequests(vault.lease(), {streamId, returnedId}).values.isEmpty()
            && !vault.pendingRequests(vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty(); }, 2000)) return false;
        const auto source = sourceResponse(); if (!source.value("ok").toBool()) return false;
        if (!controller.applyRealtimeEvent(cryptoEvent(responseDto(source)))) return false;
        cipher["payload"] = source.value("encrypted"); if (!controller.applyRealtimeEvent(cryptoEvent(cipher))) return false;
        return QTest::qWaitFor([this] { return displayed(controller, messageId).value("decrypted").toBool(); }, 2000);
    }
};

// Test-only strict viewer echo. Preserve Fixture's existing read/control handler
// verbatim; only new send scenarios opt into the complete POST/GET identity DTO.
void useCompleteSendEcho(Fixture &fixture)
{
    const auto previous = fixture.http.handler;
    fixture.http.handler = [&fixture, previous](const Request &request) {
        auto response = previous(request);
        const auto path = request.target.path();
        if ((request.method == "POST" && path == "/api/workspace/v1/messenger/messages/")
            || (request.method == "GET" && path.startsWith("/api/workspace/v1/messenger/messages/")
                && !path.endsWith('/') && response.status == 200)) {
            auto row = response.data.toObject();
            row["user_uuid"] = userId;
            fixture.savedRequests.insert(row.value("uuid").toString(), row);
            if (request.method == "POST") fixture.savedRequest = row;
            response.data = row;
        }
        return response;
    };
}

int exactRequests(const Fixture &fixture, const QByteArray &method, const QString &path)
{
    int result = 0;
    for (const auto &request : fixture.http.requests)
        if (request.method == method && request.target.path() == path) ++result;
    return result;
}

// One live source process owns its synthetic group key from an actual grant to
// cross-decryption. No group/private key is exported to the test driver.
class SourceSendPeer final {
public:
    ~SourceSendPeer() {
        if (peer.state() != QProcess::NotRunning) {
            peer.kill();
            peer.waitForFinished(2000);
        }
    }
    bool prepare(Fixture &fixture, const QString &targetTopic, const QString &expectedText, const QString &keyUuid = returnedId) {
        const auto posts = fixture.posts;
        fixture.controller.requestChatKey(keyUuid);
        if (!QTest::qWaitFor([&fixture, posts] {
            return fixture.posts == posts + 1 && !fixture.savedRequest.isEmpty()
                && !fixture.controller.encryption().value("busy").toBool();
        }, 2000)) return false;
        const auto payload = fixture.requestBody.value("payload").toObject();
        peer.start(QStringLiteral(WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE),
            {QStringLiteral(WORKSPACE_NATIVE_E2EE_PEER_SCRIPT), "--verify-native-send"});
        if (!peer.waitForStarted(2000)) return false;
        const auto outgoingBytes = expectedText.toUtf8().size();
        const auto readText = 4 * ((outgoingBytes + 48 + 2) / 3) > 213336
            ? QString("Synthetic small read before an oversized outgoing attempt") : expectedText;
        const QJsonObject input{{"project", projectId}, {"stream", streamId}, {"key", keyUuid},
            {"request", fixture.savedRequest.value("uuid")}, {"requester", userId}, {"author", Issuer},
            {"senderDevice", IssuerDevice}, {"recipientDevice", payload.value("device_id")},
            {"publicKey", payload.value("public_key")}, {"role", "owner"},
            {"plaintext", expectedText}, {"readPlaintext", readText}, {"targetTopic", targetTopic}};
        if (!writeFrame(input)) return false;
        source = readFrame();
        if (!source.value("ok").toBool() || !source.value("readyForNativeSend").toBool()) return false;
        if (!fixture.controller.applyRealtimeEvent(cryptoEvent(fixture.responseDto(source)))) return false;
        fixture.cipher["payload"] = source.value("encrypted");
        if (!fixture.controller.applyRealtimeEvent(cryptoEvent(fixture.cipher))) return false;
        return QTest::qWaitFor([&fixture, readText, keyUuid] {
            const auto receipt = fixture.vault.requestCompletionByMessage(fixture.vault.lease(),
                {streamId, keyUuid}, fixture.savedRequest.value("uuid").toString());
            return receipt.value && receipt.value->outcome == NativeChatKeyVault::Outcome::Granted
                && displayed(fixture.controller, messageId).value("decrypted").toBool()
                && displayed(fixture.controller, messageId).value("content").toString() == readText;
        }, 2000);
    }
    QJsonObject verify(const QJsonObject &payload, const QString &topic) {
        if (!writeFrame({{"kind", "verify-native-send"}, {"stream", streamId}, {"topic", topic}, {"payload", payload}})) return {};
        peer.closeWriteChannel();
        auto output = readFrame();
        if (!QTest::qWaitFor([this] { return peer.state() == QProcess::NotRunning; }, 10000)
            || peer.exitStatus() != QProcess::NormalExit || peer.exitCode() != 0
            || !peer.readAllStandardError().isEmpty() || !buffer.isEmpty()
            || !peer.readAllStandardOutput().isEmpty()) return {};
        return output;
    }
    QJsonObject source;
private:
    QProcess peer;
    QByteArray buffer;
    bool writeFrame(const QJsonObject &frame) {
        const auto bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
        return bytes.size() <= 512 * 1024 && peer.write(bytes) == bytes.size();
    }
    QJsonObject readFrame() {
        bool oversized = false;
        if (!QTest::qWaitFor([this, &oversized] {
            buffer += peer.readAllStandardOutput();
            oversized = buffer.size() > 1024 * 1024;
            return oversized || buffer.contains('\n') || peer.state() == QProcess::NotRunning;
        }, 10000) || oversized) return {};
        const auto end = buffer.indexOf('\n');
        if (end < 0) return {};
        const auto frame = buffer.left(end); buffer.remove(0, end + 1);
        QJsonParseError error;
        const auto parsed = QJsonDocument::fromJson(frame, &error);
        return error.error == QJsonParseError::NoError && parsed.isObject() ? parsed.object() : QJsonObject{};
    }
};

const QString SourceRecipient = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
const QString SourceRecipientDevice = "ffffffff-ffff-4fff-8fff-ffffffffffff";

std::shared_ptr<QHash<QString, QString>> addSourceRecipientBinding(Fixture &fixture)
{
    const auto previous = fixture.http.handler;
    const auto roles = std::make_shared<QHash<QString, QString>>();
    fixture.http.handler = [&fixture, previous, roles](const Request &request) {
        auto response = previous(request);
        if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/stream_bindings/") {
            auto rows = response.data.toArray();
            for (qsizetype index = 0; index < rows.size(); ++index) {
                auto row = rows.at(index).toObject();
                if (row.value("project_id").toString() == projectId && row.value("stream_uuid").toString() == streamId
                    && row.value("user_uuid").toString() == userId) {
                    row["role"] = fixture.stream.value("role"); rows[index] = row;
                }
            }
            rows.append(QJsonObject{{"uuid", "dddddddd-dddd-4ddd-8ddd-dddddddddddd"}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"user_uuid", SourceRecipient}, {"role", "member"}});
            roles->clear();
            for (const auto &value : rows) {
                const auto row = value.toObject();
                if (row.value("project_id").toString() == projectId && row.value("stream_uuid").toString() == streamId)
                    roles->insert(row.value("user_uuid").toString(), row.value("role").toString());
            }
            response.data = rows;
        }
        return response;
    };
    return roles;
}

QVariantMap incomingKeyRequestRow(const NativeController &controller, const QString &request)
{
    for (const auto &value : controller.encryption().value("keyRequests").toList())
        if (value.toMap().value("id").toString() == request) return value.toMap();
    return {};
}

bool hasKeyResponseMethod(const NativeController &controller, const char *signature)
{
    return controller.metaObject()->indexOfMethod(signature) >= 0;
}

// Owns the real independent Source recipient process. Only public envelopes and
// safe boolean/completion metadata cross the two frames; no key import helper.
class SourceRecipientPeer final {
public:
    ~SourceRecipientPeer() {
        if (peer.state() != QProcess::NotRunning) { peer.kill(); peer.waitForFinished(2000); }
    }
    bool prepare(Fixture &fixture, bool seedNative, const QString &key = returnedId, bool mixedOutcomes = false) {
        const auto posts = fixture.posts;
        if (seedNative) {
            fixture.controller.requestChatKey(key);
            if (!QTest::qWaitFor([&] { return fixture.posts == posts + 1 && !fixture.savedRequest.isEmpty()
                && !fixture.controller.encryption().value("busy").toBool(); }, 2000)) return false;
        }
        const auto payload = fixture.requestBody.value("payload").toObject();
        peer.start(QStringLiteral(WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE),
            {QStringLiteral(WORKSPACE_NATIVE_E2EE_PEER_SCRIPT), mixedOutcomes ? QStringLiteral("--mixed-outcomes-pair") : QStringLiteral("--verify-native-key-response")});
        if (!peer.waitForStarted(2000)) return false;
        const QJsonObject input{{"project", projectId}, {"stream", streamId}, {"key", key},
            {"author", Issuer}, {"senderDevice", IssuerDevice}, {"requester", userId},
            {"request", fixture.savedRequest.value("uuid")}, {"publicKey", payload.value("public_key")},
            {"nativeDevice", fixture.vault.device().id}, {"nativeIssuerAllowed", true}, {"seedNative", seedNative},
            {"sourceRecipient", SourceRecipient}, {"sourceDevice", SourceRecipientDevice},
            {"requestTopic", topicId}, {"targetTopic", secondTopicId}, {"plaintext", expectedText}};
        if (!writeFrame(input)) return false;
        ready = readFrame();
        if (!ready.value("ok").toBool() || !ready.value("readyForNativeKeyResponse").toBool()
            || ready.value("recipientPending").toInt() != 1 || ready.value("recipientHasKey").toBool()
            || ready.value("independentOwner").toString() != SourceRecipient
            || ready.value("independentDevice").toString() != SourceRecipientDevice) return false;
        const auto request = ready.value("request").toObject();
        requestId = request.value("uuid").toString();
        requestDto = message({}, requestId, topicId);
        requestDto["author_uuid"] = SourceRecipient; requestDto["user_uuid"] = userId;
        requestDto["is_own"] = false; requestDto["payload"] = request.value("payload");
        if (!parseMessage(requestDto, projectId)) return false;
        if (!seedNative) return true;
        const auto grant = fixture.responseDto(QJsonObject{{"message", ready.value("nativeGrant")}});
        if (!fixture.controller.applyRealtimeEvent(cryptoEvent(grant))) return false;
        fixture.cipher["payload"] = ready.value("encrypted");
        if (!fixture.controller.applyRealtimeEvent(cryptoEvent(fixture.cipher))) return false;
        return QTest::qWaitFor([&] { return displayed(fixture.controller, messageId).value("decrypted").toBool()
            && displayed(fixture.controller, messageId).value("content").toString() == expectedText; }, 2000);
    }
    QJsonObject finish(const QJsonObject &response = {}, const QString &canonical = {}) {
        const auto frame = response.isEmpty() ? QJsonObject{{"kind", "finish-without-response"}}
            : QJsonObject{{"kind", "verify-native-key-response"}, {"response", response}, {"canonical", canonical}};
        if (!writeFrame(frame)) return {};
        peer.closeWriteChannel(); const auto result = readFrame();
        if (!QTest::qWaitFor([this] { return peer.state() == QProcess::NotRunning; }, 10000)
            || peer.exitStatus() != QProcess::NormalExit || peer.exitCode() != 0
            || !peer.readAllStandardError().isEmpty() || !peer.readAllStandardOutput().isEmpty() || !buffer.isEmpty()) return {};
        return result;
    }
    QJsonObject ready, requestDto;
    QString requestId;
    const QString expectedText = "Synthetic native-issued key proves independent Source decrypt";
private:
    QProcess peer;
    QByteArray buffer;
    bool writeFrame(const QJsonObject &frame) {
        const auto bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
        return bytes.size() <= 512 * 1024 && peer.write(bytes) == bytes.size();
    }
    QJsonObject readFrame() {
        bool oversized = false;
        if (!QTest::qWaitFor([this, &oversized] {
            buffer += peer.readAllStandardOutput(); oversized = buffer.size() > 1024 * 1024;
            return oversized || buffer.contains('\n') || peer.state() == QProcess::NotRunning;
        }, 10000) || oversized) return {};
        const auto end = buffer.indexOf('\n'); if (end < 0) return {};
        const auto frame = buffer.left(end); buffer.remove(0, end + 1);
        QJsonParseError error; const auto parsed = QJsonDocument::fromJson(frame, &error);
        return error.error == QJsonParseError::NoError && parsed.isObject() ? parsed.object() : QJsonObject{};
    }
};

// The production client uses HTTPS/WSS and its real bearer subprotocol. Only
// the TLS-terminated fixture multiplexes plaintext HTTP and WebSocket upstream.
class RealtimeTransport final : public QObject {
public:
    Fixture &fixture;
    QTcpServer gateway;
    QWebSocketServer server{"Private encrypted workflow", QWebSocketServer::NonSecureMode};
    RealtimeClient client;
    QVector<QPointer<QWebSocket>> sockets;
    QMap<quint64, QJsonObject> events;
    std::function<Response(const Request &)> previous;
    const QString generation = "private-e2ee-generation";
    int eventGets = 0, snapshots = 0;
    bool duplicateRows = false, holdReady = false;
    QHash<QWebSocket *, QJsonObject> pendingReady;
    explicit RealtimeTransport(Fixture &f) : fixture(f), client(&f.api, &f.session), previous(f.http.handler) {
        gateway.listen(QHostAddress::LocalHost);
        server.setSupportedSubprotocols({"workspace.events.v1"});
        fixture.tls.upstream = gateway.serverPort();
        connect(&gateway, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = gateway.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("routed").toBool()) return;
                    const auto bytes = socket->peek(socket->bytesAvailable());
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    socket->setProperty("routed", true);
                    if (bytes.left(end).toLower().contains("upgrade: websocket")) {
                        server.handleConnection(socket);
                        return;
                    }
                    auto *upstream = new QTcpSocket(socket);
                    connect(upstream, &QTcpSocket::connected, socket, [socket, upstream] { upstream->write(socket->readAll()); });
                    connect(socket, &QTcpSocket::readyRead, upstream, [socket, upstream] {
                        if (upstream->state() == QAbstractSocket::ConnectedState) upstream->write(socket->readAll());
                    });
                    connect(upstream, &QTcpSocket::readyRead, socket, [socket, upstream] { socket->write(upstream->readAll()); });
                    connect(upstream, &QTcpSocket::disconnected, socket, &QTcpSocket::disconnectFromHost);
                    upstream->connectToHost(QHostAddress::LocalHost, fixture.http.serverPort());
                });
            }
        });
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            auto *socket = server.nextPendingConnection(); sockets.append(socket);
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            const auto cursor = QUrlQuery(socket->requestUrl()).queryItemValue("last_epoch_version").toLongLong();
            const QJsonObject ready{{"type", "ready"}, {"epoch_generation", generation}, {"epoch_version", cursor}};
            if (holdReady) pendingReady.insert(socket, ready);
            else socket->sendTextMessage(QString::fromUtf8(QJsonDocument(ready).toJson(QJsonDocument::Compact)));
        });
        connect(&client, &RealtimeClient::snapshotRequired, this, [this] { ++snapshots; });
        fixture.http.handler = [this](const Request &request) {
            if (request.target.path() == "/api/workspace/v1/epoch/")
                return Response{QJsonObject{{"epoch_version", 10}, {"current_epoch_version", 10},
                    {"minimum_epoch_version", 1}, {"epoch_generation", generation}}};
            if (request.target.path() == "/api/workspace/v1/events/") {
                ++eventGets;
                const QUrlQuery query(request.target);
                const auto after = std::max(query.queryItemValue("epoch_version>").toULongLong(),
                    query.queryItemValue("page_marker").toULongLong());
                QJsonArray rows; bool more = false;
                for (auto it = events.upperBound(after); it != events.cend(); ++it) {
                    if (rows.size() >= 2) { more = true; break; }
                    rows.append(it.value());
                    if (duplicateRows) rows.append(it.value());
                }
                Response result{rows};
                if (more) result.headers.insert("X-Pagination-Marker",
                    QByteArray::number(rows.last().toObject().value("epoch_version").toInteger()));
                return result;
            }
            return previous(request);
        };
    }
    ~RealtimeTransport() override {
        client.stop();
        for (auto &socket : sockets) if (socket) socket->abort();
        server.close(); gateway.close();
        fixture.http.handler = previous;
        fixture.tls.upstream = fixture.http.serverPort();
    }
    bool start() {
        fixture.controller.useRealtime(&client);
        return QTest::qWaitFor([this] { return fixture.controller.syncState() == "live" && !sockets.isEmpty(); }, 2000);
    }
    QJsonObject publish(quint64 epoch, const QString &kind, const QJsonObject &dto, bool wake = true) {
        auto event = cryptoEvent(dto); auto payload = event.value("payload").toObject();
        payload["kind"] = kind; event["payload"] = payload;
        event["schema_version"] = 1; event["epoch_version"] = qint64(epoch);
        event["object_type"] = kind.section('.', 0, 0); event["action"] = kind.section('.', 1);
        event["created_at"] = "2026-01-02T10:00:00Z"; event["updated_at"] = "2026-01-02T10:00:01Z";
        events.insert(epoch, event);
        if (wake) send(event);
        return event;
    }
    void releaseReady() {
        holdReady = false;
        const auto pending = std::move(pendingReady); pendingReady.clear();
        for (auto it = pending.cbegin(); it != pending.cend(); ++it)
            it.key()->sendTextMessage(QString::fromUtf8(QJsonDocument(it.value()).toJson(QJsonDocument::Compact)));
    }
    void send(const QJsonObject &event) {
        for (auto &socket : sockets) if (socket && socket->state() == QAbstractSocket::ConnectedState)
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
    }
};

// New test-only lifecycle helper; existing Fixture/peer modes stay byte-exact.
class IssuerCase final {
public:
    Fixture f;
    SourceRecipientPeer peer;
    IssuerCase() {
        f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        addSourceRecipientBinding(f); useCompleteSendEcho(f);
    }
    bool start(bool archived = false) {
        if (!f.start(archived) || !peer.prepare(f, true)) return false;
        f.savedRequests.insert(peer.requestId, peer.requestDto);
        return f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto))
            && QTest::qWaitFor([&] { return !incomingKeyRequestRow(f.controller, peer.requestId).isEmpty(); }, 2000);
    }
    QVariantMap row() const { return incomingKeyRequestRow(f.controller, peer.requestId); }
    QVariantMap response() const { return f.controller.encryption().value("keyResponse").toMap(); }
    QJsonObject finishActual() { return peer.finish(f.savedRequest, f.requestBody.value("uuid").toString()); }
};

// Add after the existing IssuerCase helper. Source-only, not built/run.
class OrdinaryRejectCase final {
public:
    std::shared_ptr<bool> requesterMember = std::make_shared<bool>(true);
    std::shared_ptr<QHash<QString, QString>> servedRoles = std::make_shared<QHash<QString, QString>>();
    Fixture f;
    SourceRecipientPeer peer;
    OrdinaryRejectCase() {
        f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        f.stream["encryption"] = false; f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        f.cipher["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Synthetic ordinary history"}};
        addSourceRecipientBinding(f); useCompleteSendEcho(f);
        const auto previous = f.http.handler;
        f.http.handler = [previous, present = requesterMember, roles = servedRoles](const Request &request) {
            auto response = previous(request);
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/stream_bindings/") {
                QJsonArray rows; roles->clear();
                for (const auto &value : response.data.toArray()) {
                    const auto row = value.toObject();
                    if (!*present && row.value("user_uuid").toString() == SourceRecipient) continue;
                    rows.append(row); roles->insert(row.value("user_uuid").toString(), row.value("role").toString());
                }
                response.data = rows;
            }
            return response;
        };
    }
    bool start() {
        if (!f.start() || !peer.prepare(f, false, OtherGeneration)) return false;
        f.savedRequests.insert(peer.requestId, peer.requestDto);
        return f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto))
            && QTest::qWaitFor([this] { return f.streamGets == 1 && f.bindingGets == 1
                && f.api.findChildren<QNetworkReply *>().isEmpty() && !row().isEmpty(); }, 2000);
    }
    QVariantMap row() const { return incomingKeyRequestRow(f.controller, peer.requestId); }
    QVariantMap response() const { return f.controller.encryption().value("keyResponse").toMap(); }
    bool pending(const QByteArray &method, const QString &path) const {
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == path
                && reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == method) return true;
        return false;
    }
    bool streamUpdated() {
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
        payload["kind"] = "stream.updated"; event["payload"] = payload;
        return f.controller.applyRealtimeEvent(event);
    }
};

// Passive outcome comparison: actual Source produces both public envelopes,
// Native receives controls only. Source recipient import is not claimed here.
class MixedOutcomeCase final {
public:
    Fixture f;
    SourceRecipientPeer peer;
    QJsonObject grant, reject;
    MixedOutcomeCase() {
        f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        addSourceRecipientBinding(f);
    }
    bool start() {
        if (!f.start() || !peer.prepare(f, true, returnedId, true)
            || !peer.ready.value("mixedOutcomePair").toBool()) return false;
        grant = f.responseDto(QJsonObject{{"message", peer.ready.value("mixedGrant")}});
        reject = f.responseDto(QJsonObject{{"message", peer.ready.value("mixedReject")}});
        if (!parseMessage(grant, projectId) || !parseMessage(reject, projectId)
            || grant.value("uuid") == reject.value("uuid")) return false;
        for (const auto &response : {grant, reject}) {
            const auto payload = response.value("payload").toObject();
            if (response.value("author_uuid").toString() != Issuer
                || payload.value("key_uuid").toString() != returnedId
                || payload.value("request_message_uuid").toString() != peer.requestId
                || payload.value("sender_device_id").toString() != IssuerDevice
                || payload.value("recipient_device_id").toString() != SourceRecipientDevice) return false;
        }
        if (grant.value("payload").toObject().value("kind").toString() != "key_grant"
            || reject.value("payload").toObject().value("kind").toString() != "key_reject") return false;
        const auto finished = peer.finish();
        return finished.value("ok").toBool() && finished.value("recipientStillPending").toBool()
            && !finished.value("recipientHasKey").toBool();
    }
    QVariantMap row() const { return incomingKeyRequestRow(f.controller, peer.requestId); }
};

class UncertainIssuerPost final {
public:
    Fixture &f;
    std::function<Response(const Request &)> previous;
    bool committed, failed = false;
    QJsonObject frozen, canonical;
    explicit UncertainIssuerPost(Fixture &fixture, bool applied) : f(fixture), previous(fixture.http.handler), committed(applied) {
        f.http.handler = [this](const Request &request) {
            auto reply = previous(request);
            const auto kind = request.body.value("payload").toObject().value("kind").toString();
            if (!failed && request.method == "POST" && (kind == "key_grant" || kind == "key_reject")) {
                failed = true; frozen = request.body; canonical = reply.data.toObject();
                if (!committed) f.savedRequests.remove(canonical.value("uuid").toString());
                reply.status = 503; reply.data = QJsonObject{};
            }
            return reply;
        };
    }
    ~UncertainIssuerPost() { f.http.handler = previous; }
};

QJsonObject updatedControl(const QJsonObject &dto) {
    auto event = cryptoEvent(dto); auto row = event.value("payload").toObject();
    row["kind"] = "message.updated"; event["payload"] = row; return event;
}

// Process-only fixture transport. The synthetic master is never an argument,
// environment value, public status field or file; only this bounded stdin frame.
class ProcessSecrets final : public SessionSecretStore {
public:
    QString address;
    QByteArray master;
    int writes = 0;
    ~ProcessSecrets() override { master.fill(0); master.clear(); }
    void read(const QString &key, Loaded done) override {
        const auto value = key == address ? master : QByteArray{};
        QTimer::singleShot(0, this, [done, value] { done(value, value.isEmpty() ? Error::NotFound : Error::None); });
    }
    void write(const QString &, const QByteArray &, Completion done) override {
        ++writes;
        QTimer::singleShot(0, this, [done] { done(Error::Denied); });
    }
    void remove(const QString &, Completion done) override { done(Error::Denied); }
};

std::atomic<int> ProcessWarnings{0};

int processWorker(QJsonObject &status)
{
    constexpr size_t MaximumFrame = 16 * 1024;
    std::array<char, MaximumFrame + 1> input{};
    const auto count = std::fread(input.data(), 1, input.size(), stdin);
    QByteArray wire(input.data(), qsizetype(count));
    std::fill(input.begin(), input.end(), 0);
    const auto scrub = qScopeGuard([&] { wire.fill(0); wire.clear(); });
    status = {{"pid", QCoreApplication::applicationPid()}, {"phase", "input"}};
    if (count > MaximumFrame || count < 4 + 32) return 2;
    const auto publicLength = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(wire.constData()));
    if (publicLength > MaximumFrame - 4 - 32 || count != 4 + publicLength + 32) return 2;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(wire.mid(4, publicLength), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return 2;
    const auto data = document.object();
    const auto mode = data.value("mode").toString();
    const QUrl server(data.value("server").toString());
    const auto directory = data.value("directory").toString();
    const auto owner = SessionVault::accountId(server, projectId, userId);
    if (!QStringList{"admit", "complete", "restore"}.contains(mode) || owner.isEmpty()
        || server.scheme() != "https" || server.host() != "127.0.0.1" || server.port() <= 0
        || !QDir::isAbsolutePath(directory)
        || !QFileInfo(directory).fileName().startsWith("cassi-process-vault-")) return 2;
    const auto certificates = QSslCertificate::fromData(QByteArray::fromBase64(data.value("certificate").toString().toLatin1()));
    if (certificates.size() != 1) return 2;
    auto tls = QSslConfiguration::defaultConfiguration();
    auto authorities = tls.caCertificates(); authorities.append(certificates);
    tls.setCaCertificates(authorities); QSslConfiguration::setDefaultConfiguration(tls);
    status["owner"] = owner; status["server"] = server.toString(); status["mode"] = mode;
    ProcessSecrets secrets;
    secrets.address = "chat-master-v1:" + owner;
    secrets.master = wire.right(32); wire.fill(0); wire.clear();
    ApiClient api; Session session(&api); NativeController controller(&api, &session);
    NativeChatKeyVault vault(directory, &secrets);
    const auto cleanup = qScopeGuard([&] { controller.useChatKeys(nullptr); vault.close(); });
    const auto phase = [&](const char *name) { status["phase"] = name; };
    phase("login");
    controller.login(server.toString(), "Synthetic", "synthetic-password", "");
    if (!QTest::qWaitFor([&] { return controller.projects().size() == 1; }, 2000)) return 3;
    phase("project"); controller.openProject(projectId);
    if (!QTest::qWaitFor([&] { return controller.streams().size() == 1; }, 2000)
        || session.user() != userId || session.project() != projectId) return 3;
    phase("history"); controller.selectStream(streamId);
    const int rows = mode == "complete" ? 3 : 1;
    if (!QTest::qWaitFor([&] { return controller.topics().size() == 2 && controller.messages().size() == rows; }, 2000)) return 3;
    phase("vault"); bool finished = false; auto opened = NativeChatKeyVault::Failure::Unavailable;
    vault.open(server, projectId, userId, "Synthetic native TLS device", [&](NativeChatKeyVault::Failure result) {
        opened = result; finished = true;
    });
    if (!QTest::qWaitFor([&] { return finished; }, 2000) || opened != NativeChatKeyVault::Failure::None
        || vault.lease().owner != owner) return 3;
    status["device"] = vault.device().id;
    if (mode != "admit" && vault.device().id != data.value("device").toString()) return 4;
    const NativeChatKeyVault::Address address{streamId, returnedId};
    phase("pending-before");
    if (mode == "complete") {
        auto pending = vault.pendingRequests(vault.lease(), address);
        const auto wipe = qScopeGuard([&] { for (auto &request : pending.values) request.clearSecrets(); });
        if (pending.failure != NativeChatKeyVault::Failure::None || pending.values.size() != 1) return 4;
        const auto &request = pending.values.first();
        if (request.localId != data.value("localId").toString() || request.messageId != data.value("publicId").toString()
            || request.targetTopic != secondTopicId || request.deviceName != vault.device().name
            || request.privateKey.size() != 32 || request.publicKey.size() != 32) return 4;
        status["restoredPending"] = true;
    }
    controller.useChatKeys(&vault);
    if (mode == "admit") {
        phase("request"); controller.requestChatKey(returnedId);
        if (!QTest::qWaitFor([&] {
            auto pending = vault.pendingRequests(vault.lease(), address);
            const auto wipe = qScopeGuard([&] { for (auto &request : pending.values) request.clearSecrets(); });
            return pending.failure == NativeChatKeyVault::Failure::None && pending.values.size() == 1
                && !pending.values.first().messageId.isEmpty() && !controller.encryption().value("busy").toBool();
        }, 2000)) return 4;
        auto pending = vault.pendingRequests(vault.lease(), address);
        const auto wipe = qScopeGuard([&] { for (auto &request : pending.values) request.clearSecrets(); });
        const auto &request = pending.values.first();
        auto key = vault.read(vault.lease(), address); const auto wipeKey = qScopeGuard([&] { key.value.bytes.fill(0); });
        if (key.failure != NativeChatKeyVault::Failure::None || !key.value.bytes.isEmpty()
            || request.privateKey.size() != 32 || request.targetTopic != secondTopicId) return 4;
        status["localId"] = request.localId; status["publicId"] = request.messageId;
        status["targetTopic"] = request.targetTopic; status["pending"] = 1;
    } else {
        phase("completion");
        const auto localId = data.value("localId").toString();
        const auto publicId = data.value("publicId").toString();
        const auto responseId = data.value("responseId").toString();
        if (!QTest::qWaitFor([&] {
            const auto completion = vault.requestCompletion(vault.lease(), address, localId);
            return completion.failure == NativeChatKeyVault::Failure::None && completion.value
                && displayed(controller, messageId).value("decrypted").toBool();
        }, 2000)) return 4;
        const auto receipt = vault.requestCompletion(vault.lease(), address, localId);
        const auto byMessage = vault.requestCompletionByMessage(vault.lease(), address, publicId);
        auto pending = vault.pendingRequests(vault.lease(), address);
        const auto wipe = qScopeGuard([&] { for (auto &request : pending.values) request.clearSecrets(); });
        auto key = vault.read(vault.lease(), address); const auto wipeKey = qScopeGuard([&] { key.value.bytes.fill(0); });
        if (!receipt.value || receipt.value->requestMessage != publicId || receipt.value->responseMessage != responseId
            || receipt.value->outcome != NativeChatKeyVault::Outcome::Granted || !byMessage.value
            || byMessage.value->localId != localId || pending.failure != NativeChatKeyVault::Failure::None
            || !pending.values.isEmpty() || key.failure != NativeChatKeyVault::Failure::None || key.value.bytes.size() != 32
            || key.value.deletionRevision != 0
            || displayed(controller, messageId).value("content").toString() != "Synthetic source-granted plaintext") return 4;
        status["receipt"] = true; status["pending"] = 0; status["decrypted"] = true;
        status["localId"] = localId; status["publicId"] = publicId; status["responseId"] = responseId;
    }
    phase("settled");
    if (!QTest::qWaitFor([&] { return api.findChildren<QNetworkReply *>().isEmpty(); }, 2000)
        || secrets.writes != 0 || ProcessWarnings.load() != 0) return 5;
    status["masterWrites"] = secrets.writes; status["warnings"] = ProcessWarnings.load();
    status["phase"] = "done"; return 0;
}

}

#ifdef WORKSPACE_NATIVE_E2EE_UI
static QQuickItem *encryptedUiItem(QQuickItem *root, const QString &name)
{
    if (root->isVisible() && root->objectName() == name) return root;
    for (auto *child : root->childItems()) if (auto *found = encryptedUiItem(child,name)) return found;
    return nullptr;
}
static QQuickItem *encryptedUiItem(QQuickWindow *window, const QString &name)
{
    if (auto *found=encryptedUiItem(window->contentItem(),name)) return found;
    for (auto *candidate : QGuiApplication::allWindows())
        if (auto *other=qobject_cast<QQuickWindow *>(candidate))
            if (other!=window) if (auto *found=encryptedUiItem(other->contentItem(),name)) return found;
    return nullptr;
}
static void encryptedUiClick(QQuickItem *target)
{
    QVERIFY(target); QVERIFY(target->isEnabled()); QVERIFY(target->isVisible());
    auto *window=target->window(); QVERIFY(window);
    QVERIFY(target->width()>0 && target->height()>0);
    QTest::mouseClick(window,Qt::LeftButton,{},target->mapToScene(QPointF(target->width()/2,target->height()/2)).toPoint());
}
static void encryptedUiMenu(QQuickWindow *window)
{
    auto *bubble=encryptedUiItem(window,"message-"+messageId); QVERIFY(bubble);
    QTest::mouseClick(bubble->window(),Qt::RightButton,{},bubble->mapToScene(QPointF(bubble->width()/2,bubble->height()/2)).toPoint());
    auto *menu=window->findChild<QObject *>("messageContextMenu");
    if (!menu) menu=window->findChild<QObject *>("messageMenu");
    QVERIFY(menu); QTRY_VERIFY(menu->property("visible").toBool() && menu->property("opened").toBool());
    auto *list=menu->property("contentItem").value<QQuickItem *>();QVERIFY(list);
    // opened precedes the first menu ListView layout; use source-sized content
    // readiness rather than dispatching into a padding-only popup.
    QTRY_VERIFY(list->property("contentHeight").toReal()>0 && menu->property("height").toReal()==list->property("contentHeight").toReal()+menu->property("topPadding").toReal()+menu->property("bottomPadding").toReal());
}
static void awaitEncryptedCopyLayout(QQuickWindow *window,QQuickItem *action)
{
    auto *menu=window->findChild<QObject *>("messageMenu");QVERIFY(menu);
    auto *list=menu->property("contentItem").value<QQuickItem *>();QVERIFY(list);
    auto *targetWindow=action->window();QVERIFY(targetWindow);
    // Reopening can expose a newly-visible Copy item at the previous menu's
    // cached y/height before its first frame. Wait for rendering and the actual
    // clipped hit rectangle, rather than clicking that stale position.
    QSignalSpy rendered(targetWindow,&QQuickWindow::frameSwapped);targetWindow->update();
    QTRY_VERIFY(!rendered.isEmpty());
    const auto ready=[&] {
        auto *popup=list->parentItem();
        if(!menu->property("visible").toBool()||!menu->property("opened").toBool()
            ||!action->isVisible()||!action->isEnabled()||action->height()!=36
            ||action->window()!=targetWindow||list->window()!=targetWindow
            ||!popup||popup->window()!=targetWindow||!list->clip())return false;
        const QRectF actionRect(action->mapToScene(QPointF{}),action->size());
        return QRectF(popup->mapToScene(QPointF{}),popup->size()).contains(actionRect)
            &&QRectF(list->mapToScene(QPointF{}),list->size()).contains(actionRect);
    };
    QTRY_VERIFY2(ready(),"The rendered Copy action must fit inside the real popup and clipped list before input");
}
// Source-derived visual controls use real delegates, not a copied QML model.
static QQuickItem *encryptedUiText(QQuickItem *root, const QString &text)
{
    if (!root->isVisible()) return nullptr;
    if (root->inherits("QQuickText") && root->property("text").toString() == text) return root;
    for (auto *child : root->childItems()) if (auto *found = encryptedUiText(child, text)) return found;
    return nullptr;
}
static int encryptedUiTextCount(QQuickItem *root, const QString &text)
{
    if (!root->isVisible()) return 0;
    int result = root->inherits("QQuickText") && root->property("text").toString() == text ? 1 : 0;
    for (auto *child : root->childItems()) result += encryptedUiTextCount(child, text);
    return result;
}
static QQuickItem *encryptedUiAvatarCircle(QQuickItem *root, int diameter)
{
    if (!root || !root->isVisible()) return nullptr;
    if (root->size() == QSizeF(diameter, diameter) && root->property("radius").isValid()
        && root->property("radius").toReal() == diameter / 2.0) return root;
    for (auto *child : root->childItems())
        if (auto *found = encryptedUiAvatarCircle(child, diameter)) return found;
    return nullptr;
}
static QJsonObject encryptedUiRect(QQuickItem *item, QQuickItem *relative)
{
    const auto point = item->mapToItem(relative, QPointF{});
    return {{"x", point.x()}, {"y", point.y()}, {"width", item->width()}, {"height", item->height()}};
}
// Test-only lifecycle candidate. sync() is not a Wayland roundtrip guarantee.
// Observe focus loss while the hidden native window still belongs to this row.
static bool closeEncryptedVisualWindow(QQuickWindow *window)
{
    const QPointer<QQuickWindow> guarded(window);
    if (!guarded)
        return true;
    const bool wasActive = guarded->isActive();
    guarded->hide();
    const bool inactive = !wasActive || QTest::qWaitFor([guarded] {
        return !guarded || (!guarded->isActive() && QGuiApplication::focusWindow() != guarded.data());
    }, 2000);
    QGuiApplication::sync();
    const bool survived = bool(guarded);
    const bool closed = guarded && guarded->close();
    QGuiApplication::sync();
    return inactive && survived && closed;
}

static QVariant serviceUiProperty(QObject *object, const char *name)
{
    const auto value = QQmlProperty(object, QString::fromLatin1(name)).read();
    return value.metaType() == QMetaType::fromType<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
}
static QVariant serviceUiEvaluate(QQuickItem *list, const QString &expression)
{
    QQmlExpression evaluate(qmlContext(list), list, expression);
    const auto value = evaluate.evaluate();
    if (evaluate.hasError()) {
        QTest::qFail(qPrintable(evaluate.error().toString()), __FILE__, __LINE__);
        return {};
    }
    return value.metaType() == QMetaType::fromType<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
}
static void expandInitialServiceGroup(QQuickWindow *window, const QString &id)
{
    // Existing R/G/J assertions stay intact; reveal their real initial header.
    // No synthetic RT, direct toggle invocation or metadata getter refresh.
    if (encryptedUiItem(window, "message-" + id)) return;
    auto *list = encryptedUiItem(window, "messageList"); QVERIFY(list);
    QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
    const auto aliases = serviceUiProperty(list, "serviceAliases").toMap();
    const auto first = aliases.value(id, id).toString();
    QTRY_VERIFY(encryptedUiItem(window, "serviceGroupToggle-" + first));
    auto *button = encryptedUiItem(window, "serviceGroupToggle-" + first); QVERIFY(button);
    QTRY_VERIFY(button->window() == window && button->height() >= 28 && button->width() > 0
        && QRectF(list->mapToScene(QPointF{}), list->size()).contains(QRectF(button->mapToScene(QPointF{}), button->size())));
    if (!button->property("expanded").toBool()) {
        QSignalSpy clicked(button, SIGNAL(clicked())); QVERIFY(clicked.isValid());
        encryptedUiClick(button); QTRY_COMPARE(clicked.size(), 1);
    }
    QTRY_VERIFY(encryptedUiItem(window, "message-" + id));
}
static QJsonObject groupingMessage(const QString &text, const QString &id, int minute,
    const QString &topic = topicId)
{
    auto row = message(text, id, topic);
    row["created_at"] = QString("2026-01-02T10:%1:00.000Z").arg(minute, 2, 10, QChar('0'));
    row["read"] = true;
    return row;
}
static QJsonObject groupingAnnouncement(const QString &id, int minute, const QString &topic = topicId)
{
    auto row = groupingMessage({}, id, minute, topic);
    row["author_uuid"] = Issuer; row["is_own"] = false;
    row["payload"] = QJsonObject{{"kind", "key_announced"}, {"key_uuid", returnedId}};
    return row;
}
static void useGroupingHistory(Fixture &f, const QJsonArray &rows)
{
    const auto previous = f.http.handler;
    f.http.handler = [previous, rows](const Request &request) {
        if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/") return Response{rows};
        return previous(request);
    };
    f.controller.selectStream(streamId); // start() left the real selected topic.
}

static void captureEncryptedUi(QQuickWindow *window, const QString &scenario, const QString &state, const QString &requestId = {}, const QString &responseId = {}, const QVariantMap &responseFacts = {})
{
    const auto directory=qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");
    if(directory.isEmpty()) return;
    QVERIFY(QDir().mkpath(directory));
    QTest::mouseMove(window,QPoint(0,0));
    QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
    const auto stem="native-encrypted-"+scenario+'-'+state;
    QVERIFY(window->grabWindow().save(QDir(directory).filePath(stem+".png")));
    QJsonObject metrics{{"scenario",scenario},{"state",state},{"applicationClass",QString(QCoreApplication::instance()->metaObject()->className())},
        {"dpr",window->devicePixelRatio()},{"graphicsApi",int(window->rendererInterface()->graphicsApi())},
        {"synthetic",true},{"serviceAcceptance",false}};
    for(const auto &name:QStringList{"encryptedConversationHeader","encryptedReadStatus","encrypted-placeholder-"+messageId})
        if(auto *part=encryptedUiItem(window,name)) {
            const auto point=part->mapToScene(QPointF{});
            metrics.insert(name,QJsonObject{{"x",point.x()},{"y",point.y()},{"width",part->width()},{"height",part->height()}});
        }
    if (!requestId.isEmpty()) {
        metrics.insert("requestId", requestId);
        for (const auto &prefix : QStringList{"keyRequestService-", "grantChatKeyButton-", "rejectChatKeyButton-"})
            if (auto *part = encryptedUiItem(window, prefix + requestId)) {
                const auto point = part->mapToScene(QPointF{});
                metrics.insert(prefix, QJsonObject{{"x",point.x()},{"y",point.y()},{"width",part->width()},{"height",part->height()}});
            }
    }
    if (!responseId.isEmpty()) {
        metrics.insert("responseId", responseId);
        for (const auto &prefix : QStringList{"keyServiceResponse-", "keyServiceResponseText-", "keyServiceResponseIcon-", "requestRejectedChatKeyButton-"})
            if (auto *part = encryptedUiItem(window, prefix + responseId)) {
                const auto point = part->mapToScene(QPointF{});
                QJsonObject row{{"x",point.x()},{"y",point.y()},{"width",part->width()},{"height",part->height()}};
                if (prefix == "keyServiceResponseText-") row.insert("text", part->property("text").toString());
                metrics.insert(prefix, row);
            }
        metrics.insert("publicResponse", QJsonObject::fromVariantMap(responseFacts));
    }
    QFile output(QDir(directory).filePath(stem+".json")); QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(QJsonDocument(metrics).toJson(QJsonDocument::Indented));
}
static void captureServiceGroupingUi(QQuickWindow *window, const QString &scenario,
    const QString &state, const QString &head, int canonicalCount)
{
    const auto directory = qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");
    if (directory.isEmpty()) return;
    captureEncryptedUi(window, scenario, state);
    QVERIFY(!QTest::currentTestFailed());
    const auto path = QDir(directory).filePath("native-encrypted-" + scenario + '-' + state + ".json");
    QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly));
    auto metrics = QJsonDocument::fromJson(input.readAll()).object(); input.close();
    metrics.insert("canonicalCount", canonicalCount);
    metrics.insert("serviceHead", head);
    auto *list = encryptedUiItem(window, "messageList"); QVERIFY(list);
    metrics.insert("renderedCount", list->property("count").toInt());
    QJsonObject parts;
    for (const auto &prefix : QStringList{"serviceGroupToggle-", "serviceGroupChevron-",
            "serviceGroupCaption-", "serviceGroupStatus-", "serviceBodyLoader-"}) {
        if (auto *item = encryptedUiItem(window, prefix + head)) {
            const auto position = item->mapToScene(QPointF{});
            QJsonObject part{{"x", position.x()}, {"y", position.y()},
                {"width", item->width()}, {"height", item->height()}};
            if (prefix == "serviceGroupToggle-") {
                part.insert("status", item->property("status").toString());
                part.insert("expanded", item->property("expanded").toBool());
                part.insert("horizontalPadding", item->property("horizontalPadding").toReal());
            } else if (prefix == "serviceGroupChevron-") part.insert("rotation", item->rotation());
            else if (prefix == "serviceBodyLoader-") {
                part.insert("active", item->property("active").toBool());
                part.insert("bodyInstantiated", bool(item->property("item").value<QObject *>()));
            } else {
                part.insert("text", item->property("text").toString());
                part.insert("fontPixelSize", item->property("font").value<QFont>().pixelSize());
                part.insert("lineHeight", item->property("lineHeight").toReal());
                part.insert("color", item->property("color").value<QColor>().name());
            }
            parts.insert(prefix, part);
        }
    }
    metrics.insert("grouping", parts);
    QFile output(path); QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(QJsonDocument(metrics).toJson(QJsonDocument::Indented));
}

#endif

// Source-only TLS fixture extension: real collection GET and real worker disk;
// no Source peer regeneration, key injection, UI or production test seam.
class MissingRequestCase final {
public:
    QTemporaryDir cacheDirectory;
    QSemaphore gateRelease;
    std::atomic_bool blockClock{false}, clockEntered{false};
    NativeCacheService cache{cacheDirectory.path(), {}, false, [this] {
        if (blockClock.exchange(false)) { clockEntered = true; gateRelease.acquire(); }
        return qint64(10000);
    }}; // clock/gate/cache all outlive Fixture/controller
    Fixture f;
    QString requestId = "eeeeeeee-eeee-4eee-8eee-000000000001";
    QJsonObject request;
    QJsonValue lookupRows;
    int lookupCount = 0, lookupStatus = 200, lookupDelay = 1200, maxActive = 0;
    QHash<QString, QJsonObject> additionalRequests;
    bool exactQuery = true;
    std::function<Response(const Request &)> previous;
    MissingRequestCase() {
        request = message({}, requestId, secondTopicId); request["user_uuid"] = userId;
        request["author_uuid"] = SourceRecipient; request["is_own"] = false;
        request["payload"] = QJsonObject{{"kind", "key_request"}, {"key_uuid", returnedId},
            {"device_id", "resolver-recipient-device"}, {"device_name", "Resolver recipient device"},
            {"public_key", QString::fromLatin1(QByteArray(32, 'p').toBase64())}};
        f.cipher["user_uuid"] = userId;
        f.cipher["payload"] = QJsonObject{{"kind", "key_reject"}, {"key_uuid", returnedId},
            {"request_message_uuid", requestId}, {"sender_device_id", IssuerDevice},
            {"recipient_device_id", "resolver-recipient-device"}};
        lookupRows = QJsonArray{request}; previous = f.http.handler;
        f.http.handler = [this](const Request &wire) -> Response {
            if (wire.target.path() == "/resolver-unrelated") return {QJsonObject{{"ok", true}}, 200, 1200};
            const QUrlQuery query(wire.target);
            if (wire.method == "GET" && wire.target.path() == "/api/workspace/v1/messenger/messages/" && query.hasQueryItem("uuid")) {
                ++lookupCount;
                const auto requested = query.queryItemValue("uuid");
                exactQuery = exactQuery && (requested == requestId || additionalRequests.contains(requested)) && query.queryItemValue("project_id") == projectId
                    && !query.hasQueryItem("stream_uuid") && !query.hasQueryItem("topic_uuid") && query.queryItems().size() == 2;
                int active = 0;
                for (const auto reply : f.api.findChildren<QNetworkReply *>())
                    if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/messages/"
                        && QUrlQuery(reply->url()).hasQueryItem("uuid")) ++active;
                maxActive = std::max(maxActive, active);
                return {additionalRequests.contains(requested) ? QJsonValue(QJsonArray{additionalRequests.value(requested)}) : lookupRows,
                    lookupStatus, lookupDelay};
            }
            return previous(wire);
        };
        f.controller.useCache(&cache);
    }
    ~MissingRequestCase() { f.http.handler = previous; gateRelease.release(); } // no dangling state during Fixture/Vault teardown
    bool start() {
        const bool started = f.start();
        const bool complete = started && QTest::qWaitFor([this] {
            // useChatKeys queues the loaded G/J control work on a zero timer.
            // Wait for its real authority cycle, not a moment before dispatch.
            return cache.available() && f.streamGets == 1 && f.bindingGets == 1
                && f.api.findChildren<QNetworkReply *>().isEmpty();
        }, 2000);
        if (!complete) {
            QJsonArray pending;
            for (auto *reply : f.api.findChildren<QNetworkReply *>())
                pending.append(QJsonObject{{"path", reply->url().path()}, {"finished", reply->isFinished()}});
            qInfo().noquote() << "MISSING_START_DIAGNOSTIC"
                << QJsonDocument(QJsonObject{{"fixtureStarted", started}, {"cacheAvailable", cache.available()},
                    {"streamGets", f.streamGets}, {"bindingGets", f.bindingGets}, {"pending", pending}}).toJson(QJsonDocument::Compact);
        }
        return complete;
    }
    bool hasApi() const { return f.controller.metaObject()->indexOfMethod("resolveKeyServiceRequest(QString)") >= 0; }
    bool demand(const QString &response = messageId) {
        return QMetaObject::invokeMethod(&f.controller, "resolveKeyServiceRequest", Qt::DirectConnection, Q_ARG(QString, response));
    }
    QVariantMap metadata(const QString &response = messageId) const { return displayed(f.controller, response).value("keyServiceResponse").toMap(); }
    bool pending() const {
        for (const auto reply : f.api.findChildren<QNetworkReply *>()) {
            const QUrlQuery query(reply->url());
            if (!reply->isFinished() && reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "GET"
                && reply->url().path() == "/api/workspace/v1/messenger/messages/" && query.queryItemValue("uuid") == requestId) return true;
        }
        return false;
    }
    void holdHit(const QString &id) {
        clockEntered = false; blockClock = true;
        cache.getMessageBody(cache.lease(), id, &f.controller, [](auto) {});
    }
    bool putBody(const QJsonObject &row) {
        auto done = std::make_shared<int>(0);
        if (!cache.putMessageBody(cache.lease(), row, &f.controller, [done](bool okay) { *done = okay ? 1 : -1; })) return false;
        return QTest::qWaitFor([done] { return *done != 0; }, 2000) && *done == 1;
    }
    bool putHistory(const NativeCache::Key &key, const QJsonObject &row) {
        auto done = std::make_shared<int>(0);
        if (!cache.put(cache.lease(), key, {row}, "preserved-cursor", &f.controller, [done](bool okay) { *done = okay ? 1 : -1; })) return false;
        return QTest::qWaitFor([done] { return *done != 0; }, 2000) && *done == 1;
    }
    bool flush() {
        auto done = std::make_shared<int>(0); cache.flush(&f.controller, [done](bool okay) { *done = okay ? 1 : -1; });
        return QTest::qWaitFor([done] { return *done != 0; }, 2000) && *done == 1;
    }
    std::optional<NativeCache::MessageBody> diskBody() {
        struct State { bool done = false; std::optional<NativeCache::MessageBody> body; };
        const auto result = std::make_shared<State>();
        cache.getMessageBody(cache.lease(), requestId, &f.controller, [result](auto body) { result->body = body; result->done = true; });
        if (!QTest::qWaitFor([result] { return result->done; }, 2000)) return {};
        return result->body;
    }
};

bool discloseMissingRequest(MissingRequestCase &c, quint64 serial, const QString &response = messageId)
{
    return QMetaObject::invokeMethod(&c.f.controller, "resolveKeyServiceRequest", Qt::DirectConnection,
        Q_ARG(QString, response), Q_ARG(quint64, serial));
}
bool missingRequestTransportBarrier(MissingRequestCase &c)
{
    // Real existing delayed endpoint drives Qt transport, not a sleep/getter side effect.
    const auto done = std::make_shared<bool>(false);
    c.f.api.request("GET", "/resolver-unrelated", {}, {}, [done](const ApiResult &result) { *done = result.ok(); });
    return QTest::qWaitFor([done] { return *done; }, 2000);
}


#ifdef WORKSPACE_NATIVE_E2EE_UI
// Add inside the existing WORKSPACE_NATIVE_E2EE_UI helper section, after the
// independently reviewed MissingRequestCase. Never use useGroupingHistory here:
// its old whole-collection override would also swallow the uuid-filtered GET.
static bool useMissingRequestUiHistory(MissingRequestCase &c, const QJsonArray &rows)
{
    // Diagnostic observations are read only, never dispatch or alter the cache.
    const auto historyGets=[&c] {
        int count=0;
        for(const auto &wire:c.f.http.requests) {
            const QUrlQuery query(wire.target);
            if(wire.method=="GET" && wire.target.path()=="/api/workspace/v1/messenger/messages/"
                && !query.hasQueryItem("uuid") && !query.hasQueryItem("topic_uuid")
                && query.queryItemValue("stream_uuid")==streamId)++count;
        }
        return count;
    };
    const auto beforeGets=historyGets();
    const auto beforeCommit=c.f.controller.historyCommitRevision();
    const auto initial=c.f.controller.messages();
    const auto previous = c.f.http.handler;
    c.f.http.handler = [previous, rows](const Request &request) {
        if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/"
            && !QUrlQuery(request.target).hasQueryItem("uuid")) return Response{rows};
        return previous(request);
    };
    // Fixture changed the server snapshot after start() warmed this aggregate.
    // Retain all bodies/history rows, but require one real refresh of this stream.
    if(!c.cache.markStale(c.cache.lease(),NativeCache::Kind::History,streamId) || !c.flush())return false;
    c.f.controller.selectStream(streamId);
    const bool ready=QTest::qWaitFor([&c, rows, &historyGets, beforeGets] {
        const auto loaded = c.f.controller.messages();
        if (historyGets()<=beforeGets || c.f.controller.conversationKind() != "stream" || !c.f.controller.selectedTopic().isEmpty()
            || c.f.controller.busy() || loaded.size() != rows.size() || !c.f.controller.messageHistoryReady()
            || !c.f.api.findChildren<QNetworkReply *>().isEmpty()) return false;
        for (const auto &raw : rows) {
            const auto id = raw.toObject().value("uuid").toString();
            if (std::none_of(loaded.cbegin(), loaded.cend(), [&id](const QVariant &entry) {
                return entry.toMap().value("id").toString() == id;
            })) return false;
        }
        return true;
    }, 2000);
    if(!ready) {
        const auto loaded=c.f.controller.messages();
        bool expectedPresent=false,initialPresent=false,rawMatchesFixture=false,expectedRawId=false;
        int grants=0,rejects=0,encrypted=0,knownRecipient=0,currentDevice=0,expectedRequestReferences=0;
        for(const auto &raw:rows) {
            const auto row=raw.toObject();
            rawMatchesFixture=rawMatchesFixture||row==c.f.cipher;
            expectedRawId=expectedRawId||row.value("uuid")==c.f.cipher.value("uuid");
        }
        for(const auto &value:loaded) {
            const auto row=value.toMap();
            expectedPresent=expectedPresent||row.value("id").toString()==c.f.cipher.value("uuid").toString();
            for(const auto &prior:initial)initialPresent=initialPresent||row.value("id")==prior.toMap().value("id");
            grants+=row.value("kind").toString()=="key_grant";
            rejects+=row.value("kind").toString()=="key_reject";
            encrypted+=row.value("kind").toString()=="e2ee";
            const auto facts=row.value("keyServiceResponse").toMap();
            knownRecipient+=facts.value("recipientKnown").toBool();
            currentDevice+=facts.value("ownCurrentDevice").toBool();
            expectedRequestReferences+=facts.value("requestId").toString()==c.requestId;
        }
        qInfo().noquote()<<"MISSING_REQUEST_UI_HISTORY_FAILED"<<QJsonDocument(QJsonObject{
            {"aggregateHistoryGetsBefore",beforeGets},{"aggregateHistoryGetsAfter",historyGets()},
            {"aggregateHistoryGetDelta",historyGets()-beforeGets},
            {"historyCommitDelta",qint64(c.f.controller.historyCommitRevision()-beforeCommit)},
            {"initialRows",initial.size()},{"loadedRows",loaded.size()},{"expectedRawRows",rows.size()},
            {"expectedLoadedIdPresent",expectedPresent},{"retainedInitialIdPresent",initialPresent},
            {"rawFixtureUuidInExpectedRows",expectedRawId},{"rawFixtureDtoInExpectedRows",rawMatchesFixture},
            {"rawFixtureIsGrant",c.f.cipher.value("payload").toObject().value("kind").toString()=="key_grant"},
            {"rawFixtureIsReject",c.f.cipher.value("payload").toObject().value("kind").toString()=="key_reject"},
            {"loadedGrantRows",grants},{"loadedRejectRows",rejects},{"loadedEncryptedRows",encrypted},
            {"loadedKnownRecipientRows",knownRecipient},{"loadedOwnCurrentDeviceRows",currentDevice},
            {"loadedExpectedRequestReferences",expectedRequestReferences},{"lookupCount",c.lookupCount},
            {"historyReady",c.f.controller.messageHistoryReady()},{"controllerErrorPresent",!c.f.controller.error().isEmpty()}
        }).toJson(QJsonDocument::Compact);
    }
    return ready;
}

static bool prepareOwnMissingRequestUi(MissingRequestCase &c, bool grant)
{
    // Failure-only diagnostics: constant stage + counts/booleans. Never print
    // IDs, provider output, payload, error text, headers or key material.
    QElapsedTimer elapsed; elapsed.start();
    const auto fail=[&](const char *stage) {
        const auto encryption=c.f.controller.encryption();
        const auto completion=c.f.vault.requestCompletionByMessage(c.f.vault.lease(),{streamId,returnedId},c.requestId);
        auto pending=c.f.vault.pendingRequests(c.f.vault.lease(),{streamId,returnedId});
        const auto wipe=qScopeGuard([&pending] {for(auto &row:pending.values)row.clearSecrets();});
        bool bound=false,matchingDevice=false;
        for(const auto &row:pending.values) {
            bound=bound||row.messageId==c.requestId;
            matchingDevice=matchingDevice||row.deviceName==c.request.value("payload").toObject().value("device_name").toString();
        }
        int active=0,finished=0;
        for(const auto reply:c.f.api.findChildren<QNetworkReply *>())reply->isFinished()?++finished:++active;
        const QJsonObject facts{{"stage",QString::fromLatin1(stage)},{"elapsedMs",elapsed.elapsed()},
            {"posts",c.f.posts},{"streamGets",c.f.streamGets},{"bindingGets",c.f.bindingGets},
            {"topicGets",c.f.topicGets},{"catalogGets",c.f.catalogGets},{"lookupCount",c.lookupCount},
            {"activeReplies",active},{"finishedReplies",finished},{"loadedRows",c.f.controller.messages().size()},
            {"historyReady",c.f.controller.messageHistoryReady()},{"controllerBusy",c.f.controller.busy()},
            {"authenticated",c.f.controller.authenticated()},{"aggregateRoute",c.f.controller.selectedStream()==streamId&&c.f.controller.selectedTopic().isEmpty()},
            {"encryptionBusy",encryption.value("busy").toBool()},{"encryptionErrorPresent",!encryption.value("error").toString().isEmpty()},
            {"phaseRequested",encryption.value("state").toString()=="requested"},{"vaultAvailable",c.f.vault.available()},
            {"savedCanonicalRequestPresent",!c.f.savedRequest.isEmpty()},{"requestParsed",parseMessage(c.request,projectId).has_value()},
            {"responseParsed",parseMessage(c.f.cipher,projectId).has_value()},
            {"completionFailure",int(completion.failure)},{"completionPresent",completion.value.has_value()},
            {"completionMatchesExpectedOutcome",completion.value&&completion.value->outcome==(grant?NativeChatKeyVault::Outcome::Granted:NativeChatKeyVault::Outcome::Rejected)},
            {"pendingFailure",int(pending.failure)},{"pendingCount",pending.values.size()},
            {"pendingBoundToExpectedR",bound},{"pendingDeviceNameMatchesR",matchingDevice},
            {"responseRecipientMatchesVault",c.f.cipher.value("payload").toObject().value("recipient_device_id").toString()==c.f.vault.device().id},
            {"responseReferencesExpectedR",c.f.cipher.value("payload").toObject().value("request_message_uuid").toString()==c.requestId}};
        qInfo().noquote()<<"MISSING_REQUEST_UI_PREP_FAILED"<<QJsonDocument(facts).toJson(QJsonDocument::Compact);
        return false;
    };
    c.f.stream = fullEncryptedStream(); useCompleteSendEcho(c.f);
    if (!c.start()) return fail("start");
    c.f.controller.requestChatKey(returnedId);
    if (!QTest::qWaitFor([&c] { return c.f.posts == 1 && !c.f.controller.encryption().value("busy").toBool(); }, 2000)) return fail("native-request");
    // An actual Native canonical request and actual pinned Source G/J. The R
    // has not been injected into loaded history or the public correlation map.
    c.request = c.f.savedRequest; c.request["user_uuid"] = userId;
    c.request["reaction_users"] = QJsonObject{};
    c.request["pinned"] = false; c.request["reactions"] = QJsonObject{};
    c.request["updated_at"] = c.request.value("created_at");
    c.requestId = c.request.value("uuid").toString();
    if (!parseMessage(c.request, projectId) || c.requestId.isEmpty()) return fail("request-parse");
    const auto source = c.f.sourceResponse(!grant);
    if (!source.value("ok").toBool()) return fail("source-response");
    c.f.cipher = c.f.responseDto(source); c.f.cipher["user_uuid"] = userId;
    c.f.cipher["reaction_users"] = QJsonObject{};
    c.f.cipher["pinned"] = false; c.f.cipher["reactions"] = QJsonObject{};
    c.f.cipher["updated_at"] = c.f.cipher.value("created_at");
    c.lookupRows = QJsonArray{c.request};
    if (!parseMessage(c.f.cipher, projectId)) return fail("response-parse");
    if (!useMissingRequestUiHistory(c, QJsonArray{c.f.cipher})) return fail("history-ready");
    // Positive initial authority/recipient completion, before measuring resolver
    // API cost. Do not mistake deferred first control work for a resolver GET.
    const bool completed=QTest::qWaitFor([&c, grant] {
        const auto completion=c.f.vault.requestCompletionByMessage(c.f.vault.lease(),{streamId,returnedId},c.requestId);
        return completion.failure==NativeChatKeyVault::Failure::None && completion.value
            && completion.value->outcome==(grant?NativeChatKeyVault::Outcome::Granted:NativeChatKeyVault::Outcome::Rejected)
            && c.f.api.findChildren<QNetworkReply *>().isEmpty();
    },2000);
    if (!completed) return fail("vault-completion");
    if (c.lookupCount!=0) return fail("unexpected-lookup");
    return true;
}

static bool prepareExternalMissingRequestUi(MissingRequestCase &c, const MixedOutcomeCase &source, bool both = false)
{
    c.f.stream = fullEncryptedStream();
    c.request = source.peer.requestDto; c.request["user_uuid"] = userId;
    c.request["topic_uuid"] = secondTopicId; // Source correlation is same-stream, not same-topic.
    c.requestId = c.request.value("uuid").toString(); c.lookupRows = QJsonArray{c.request};
    c.f.cipher = source.reject; c.f.cipher["user_uuid"] = userId; c.f.cipher["topic_uuid"] = topicId;
    if (!parseMessage(c.request, projectId) || !parseMessage(c.f.cipher, projectId) || !c.start()) return false;
    if (!both) return c.lookupCount == 0;
    auto first = source.grant; first["user_uuid"] = userId; first["topic_uuid"] = topicId;
    first["created_at"] = "2026-01-02T10:01:00.000Z";
    c.f.cipher["created_at"] = "2026-01-02T10:02:00.000Z";
    return parseMessage(first, projectId).has_value()
        && useMissingRequestUiHistory(c, QJsonArray{first, c.f.cipher}) && c.lookupCount == 0;
}

static QString missingRequestUiExpectedTitle(const MissingRequestCase &c, bool grant, bool own, const QString &locale)
{
    const auto device = c.request.value("payload").toObject().value("device_name").toString();
    if (own) return (grant
        ? (locale == "ru" ? QString("#aaaaaaaa передал(а) вам ключ безопасности для устройства «%1»")
                          : QString("#aaaaaaaa sent you a security key for device “%1”"))
        : (locale == "ru" ? QString("#aaaaaaaa отклонил(а) ваш запрос ключа безопасности для устройства «%1»")
                          : QString("#aaaaaaaa rejected your security key request for device “%1”"))).arg(device);
    return (locale == "ru" ? QString("#aaaaaaaa отклонил(а) запрос ключа безопасности пользователя #eeeeeeee для устройства «%1»")
                            : QString("#aaaaaaaa rejected the security key request from #eeeeeeee for device “%1”")).arg(device);
}

static void keyMissingRequestDisclosure(QQuickWindow *window, QQuickItem *header, int key)
{
    QVERIFY(header && header->window() == window && header->isVisible() && header->isEnabled());
    header->forceActiveFocus(Qt::TabFocusReason); QTRY_VERIFY(header->hasActiveFocus());
    QSignalSpy clicked(header, SIGNAL(clicked())); QVERIFY(clicked.isValid());
    QTest::keyClick(window, Qt::Key(key)); QTRY_COMPARE(clicked.size(), 1);
}

static QString missingRequestUiTitle(QQuickWindow *window, const QString &id)
{
    const auto *title=encryptedUiItem(window,"keyServiceResponseText-"+id);
    return title ? title->property("text").toString() : QString{};
}

// Optional only: records actual public fixture responses; never dispatches a
// request. Construct before prepareOwnMissingRequestUi, destroy before its c.
class MissingRequestPublicCapture final {
    MissingRequestCase &c;
    bool enabled = !qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY").isEmpty();
    std::function<Response(const Request &)> previous;
    QHash<QString,QJsonValue> actual;
    QJsonArray observations;
public:
    explicit MissingRequestPublicCapture(MissingRequestCase &fixture) : c(fixture) {
        if (!enabled) return;
        previous=c.f.http.handler;
        c.f.http.handler=[this](const Request &wire) {
            const auto response=previous(wire);
            const auto path=wire.target.path();
            if (wire.method=="GET" && response.status==200 && QStringList{
                    "/api/workspace/v1/messenger/streams/", "/api/workspace/v1/messenger/streams/"+streamId,
                    "/api/workspace/v1/messenger/stream_topics/", "/api/workspace/v1/messenger/stream_bindings/",
                    "/api/workspace/v1/users/"}.contains(path)) {
                actual.insert(path,response.data);
                observations.append(QJsonObject{{"method","GET"},{"path",path},{"status",response.status}});
            }
            return response;
        };
    }
    ~MissingRequestPublicCapture() { if(enabled)c.f.http.handler=previous; }
    void write(QQuickWindow *window,const QString &locale,const QString &mode,bool grant,const QJsonObject &verifiedWorkerBody) {
        if (!enabled) return;
        const auto directory=qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");
        QVERIFY(window); QCOMPARE(window->size(),QSize(1440,900)); QCOMPARE(window->devicePixelRatio(),qreal(1));
        QCOMPARE(window->rendererInterface()->graphicsApi(),QSGRendererInterface::OpenGL);
        const auto id=c.f.cipher.value("uuid").toString();
        QCOMPARE(c.f.controller.selectedStream(),streamId); QVERIFY(c.f.controller.selectedTopic().isEmpty());
        QVERIFY(c.f.controller.messageHistoryReady()); QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.lookupCount,1); QVERIFY(c.exactQuery);
        auto expectedWorkerBody=c.request; expectedWorkerBody.remove("pinned"); expectedWorkerBody.remove("updated_at");
        QVERIFY(!verifiedWorkerBody.contains("pinned")); QVERIFY(!verifiedWorkerBody.contains("updated_at"));
        QCOMPARE(verifiedWorkerBody.value("payload"),c.request.value("payload"));
        QCOMPARE(verifiedWorkerBody,expectedWorkerBody);
        QCOMPARE(c.f.controller.messages().size(),1); QVERIFY(displayed(c.f.controller,c.requestId).isEmpty());
        const auto facts=c.metadata(id); QVERIFY(facts.value("recipientKnown").toBool());
        auto *title=encryptedUiItem(window,"keyServiceResponseText-"+id); QVERIFY(title);
        QCOMPARE(title->property("text").toString(),missingRequestUiExpectedTitle(c,grant,true,locale));
        const auto topics=actual.value("/api/workspace/v1/messenger/stream_topics/");
        const auto bindings=actual.value("/api/workspace/v1/messenger/stream_bindings/");
        const auto users=actual.value("/api/workspace/v1/users/");
        QVERIFY(topics.isArray() && bindings.isArray() && users.isArray());
        auto catalog=actual.value("/api/workspace/v1/messenger/streams/"+streamId).toObject();
        if(catalog.isEmpty())for(const auto &row:actual.value("/api/workspace/v1/messenger/streams/").toArray())
            if(row.toObject().value("uuid").toString()==streamId)catalog=row.toObject();
        QCOMPARE(catalog,c.f.stream);
        bool issuerAllowed=catalog.value("owner").toString()==c.f.cipher.value("author_uuid").toString();
        for(const auto &value:bindings.toArray()) {
            const auto row=value.toObject();
            if(row.value("project_id").toString()==projectId && row.value("stream_uuid").toString()==streamId
                && row.value("user_uuid").toString()==c.f.cipher.value("author_uuid").toString())
                issuerAllowed=issuerAllowed||QStringList{"owner","administrator"}.contains(row.value("role").toString());
        }
        const auto lease=c.f.vault.lease(); const NativeChatKeyVault::Address address{streamId,returnedId};
        const auto completion=c.f.vault.requestCompletionByMessage(lease,address,c.requestId);
        QVERIFY(completion.failure==NativeChatKeyVault::Failure::None && completion.value);
        QCOMPARE(completion.value->requestMessage,c.requestId); QCOMPARE(completion.value->responseMessage,id);
        QCOMPARE(completion.value->outcome,grant?NativeChatKeyVault::Outcome::Granted:NativeChatKeyVault::Outcome::Rejected);
        auto pending=c.f.vault.pendingRequests(lease,address);
        const auto clear=qScopeGuard([&pending] {for(auto &row:pending.values)row.clearSecrets();});
        QCOMPARE(pending.failure,NativeChatKeyVault::Failure::None);
        // Only these derived booleans leave the vault; no PendingRequest object,
        // local UUID, public/private pair or Source provider result is serialized.
        const bool requested=!pending.values.isEmpty();
        const auto encryption=c.f.controller.encryption();
        QJsonObject labels{{userId,c.f.controller.account().value("username").toString()},
            {c.f.cipher.value("author_uuid").toString(),facts.value("senderName").toString()},
            {c.request.value("author_uuid").toString(),facts.value("recipientName").toString()}};
        const auto scenario=locale+'-'+mode,state=grant?QString("missing-r-grant-resolved"):QString("missing-r-reject-resolved");
        captureEncryptedUi(window,scenario,state,c.requestId,id,facts); QVERIFY(!QTest::currentTestFailed());
        const auto stem="native-encrypted-"+scenario+'-'+state;
        const auto png=QDir(directory).filePath(stem+".png"),metricsPath=QDir(directory).filePath(stem+".json");
        const auto packPath=QDir(directory).filePath(stem+".public-inputs.json");
        QJsonObject pack{{"schema","native-missing-r-public-v1"},{"synthetic",true},
            {"sourceProvenance",QJsonObject{{"actualNativeCase",QString::fromLatin1(QTest::currentTestFunction())+':'+QString::fromLatin1(QTest::currentDataTag())},
                {"state","Actual fixture/capture inputs only; Root must attach source/binary hashes externally"}}},
            {"ids",QJsonObject{{"project",projectId},{"user",userId},{"stream",streamId},{"topic",c.f.cipher.value("topic_uuid")}}},
            {"viewerName",c.f.controller.account().value("username").toString()},{"viewerDeviceId",c.f.vault.device().id},
            {"authorLabels",labels},{"request",c.request},{"response",c.f.cipher},
            {"messages",QJsonArray{c.f.cipher}},{"catalog",catalog},{"topics",topics},{"bindings",bindings},{"users",users},
            {"presentationState",QJsonObject{{"ownCurrentDevice",facts.value("ownCurrentDevice").toBool()},
                {"hasKey",facts.value("hasKey").toBool()},{"canRequest",encryption.value("canRequest").toBool()},
                {"issuerAllowed",issuerAllowed},{"requested",requested},{"outcome",grant?"granted":"rejected"}}},
            {"expected",QJsonObject{{"title",QJsonObject{{"en",missingRequestUiExpectedTitle(c,grant,true,"en")},
                {"ru",missingRequestUiExpectedTitle(c,grant,true,"ru")}}}}}};
        QVERIFY(c.lookupRows.isArray()); QCOMPARE(c.lookupRows.toArray(),QJsonArray{c.request});
        QCOMPARE(pack.value("request").toObject(),c.request);
        QFile packFile(packPath); QVERIFY(packFile.open(QIODevice::WriteOnly));
        const auto packBytes=QJsonDocument(pack).toJson(QJsonDocument::Indented); QCOMPARE(packFile.write(packBytes),qint64(packBytes.size()));packFile.close();
        QFile input(metricsPath); QVERIFY(input.open(QIODevice::ReadOnly));auto metrics=QJsonDocument::fromJson(input.readAll()).object();input.close();
        metrics.insert("viewport",QJsonObject{{"width",window->width()},{"height",window->height()},{"dpr",window->devicePixelRatio()}});
        metrics.insert("publicInputs",QFileInfo(packPath).fileName());metrics.insert("exactLookupGets",c.lookupCount);
        metrics.insert("exactProjectUuidQuery",c.exactQuery);
        metrics.insert("verifiedWorkerBodyMatchesNativeProjection",verifiedWorkerBody==expectedWorkerBody);
        metrics.insert("canonicalTransportRequestMatchesPublicPack",c.lookupRows.toArray()==QJsonArray{pack.value("request")});
        metrics.insert("visibleMessageCount",c.f.controller.messages().size());metrics.insert("requestVisible",false);
        metrics.insert("actualCatalogResponses",observations);metrics.insert("observedVaultCompletion",grant?"granted":"rejected");
        QFile output(metricsPath);QVERIFY(output.open(QIODevice::WriteOnly));const auto metricBytes=QJsonDocument(metrics).toJson(QJsonDocument::Indented);
        QCOMPARE(output.write(metricBytes),qint64(metricBytes.size()));output.close();
        const auto digest=[](const QString &path) {QFile file(path);if(!file.open(QIODevice::ReadOnly))return QString{};
            return QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());};
        const auto pngHash=digest(png),metricsHash=digest(metricsPath),packHash=digest(packPath);
        QVERIFY(!pngHash.isEmpty()&&!metricsHash.isEmpty()&&!packHash.isEmpty());
        const QJsonObject receipt{{"schema","native-missing-r-capture-inputs-v1"},{"synthetic",true},
            {"nativePng",png},{"nativePngSha256",pngHash},{"nativeMetrics",metricsPath},{"nativeMetricsSha256",metricsHash},
            {"publicPack",packPath},{"publicPackSha256",packHash},
            {"state","Actual capture; source/binary provenance remains Root-owned external receipt"}};
        QFile receiptFile(QDir(directory).filePath(stem+".inputs-receipt.json"));QVERIFY(receiptFile.open(QIODevice::WriteOnly));
        const auto receiptBytes=QJsonDocument(receipt).toJson(QJsonDocument::Indented);QCOMPARE(receiptFile.write(receiptBytes),qint64(receiptBytes.size()));
    }
};

// Optional passive observer. It wraps only this new UI3 handler, never requests
// data or opens/reads a Vault. Canonical rows are enriched BEFORE transport.
class AccountPublicCapture final {
    MissingRequestCase &c;
    const bool enabled = !qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY").isEmpty();
    std::function<Response(const Request &)> previous;
    QHash<QString,QJsonValue> actual;
    QJsonArray observations;
    QJsonObject lookup,initial;
    QString title,expandedStem;
    static bool completeTransport(const QJsonObject &row) {
        for(const auto *field:{"uuid","project_id","stream_uuid","topic_uuid","author_uuid","user_uuid","created_at","updated_at"})
            if(!row.value(field).isString()||row.value(field).toString().isEmpty())return false;
        for(const auto *field:{"read","pinned","starred","is_own"})if(!row.value(field).isBool())return false;
        return row.value("payload").isObject()&&row.value("reactions").isObject()&&row.value("reaction_users").isObject();
    }
    QJsonObject snapshot(const QString &path) const {
        const bool observed=actual.contains(path);
        return {{"observed",observed},{"body",observed?actual.value(path):QJsonValue(QJsonValue::Null)}};
    }
    static QString stem(bool external,bool grant) {
        return QString("account-%1-%2-en-light").arg(external?"external":"native",grant?"grant":"reject");
    }
public:
    explicit AccountPublicCapture(MissingRequestCase &fixture):c(fixture) {
        if(!enabled)return;
        previous=c.f.http.handler;
        c.f.http.handler=[this](const Request &wire) {
            const auto response=previous(wire);const auto path=wire.target.path();const QUrlQuery query(wire.target);
            if(wire.method=="GET"&&response.status==200) {
                if(QStringList{"/api/workspace/v1/messenger/streams/","/api/workspace/v1/messenger/streams/"+streamId,
                    "/api/workspace/v1/messenger/stream_topics/","/api/workspace/v1/messenger/stream_bindings/","/api/workspace/v1/users/"}.contains(path)) {
                    actual.insert(path,response.data);
                    observations.append(QJsonObject{{"method","GET"},{"path",path},{"status",response.status}});
                } else if(path=="/api/workspace/v1/messenger/messages/") {
                    QJsonArray items;for(const auto &item:query.queryItems())items.append(QJsonArray{item.first,item.second});
                    if(query.queryItemValue("uuid")==c.requestId)
                        lookup={{"method","GET"},{"path",path},{"query",items},{"status",response.status},{"body",response.data}};
                    else if(!query.hasQueryItem("uuid"))actual.insert("history",response.data);
                }
            }
            return response; // exact existing MockServer/defer gate response
        };
    }
    ~AccountPublicCapture(){if(enabled)c.f.http.handler=previous;}
    void recordInitial(const QVariantMap &session,bool live,bool selection,qulonglong revision) {
        if(!enabled)return;
        initial={{"ready",session.value("ready").toBool()},{"liveEnabled",live},{"selectionMode",selection},
            {"seenCount",session.value("seen").toMap().size()},{"preferenceCount",session.value("preferences").toMap().size()},
            {"latestNull",session.value("latest").isNull()},{"historyCommitRevision",QString::number(revision)}};
        QVERIFY(initial.value("ready").toBool()&&live&&!selection);
        QCOMPARE(initial.value("seenCount").toInt(),0);QCOMPARE(initial.value("preferenceCount").toInt(),0);
        QVERIFY(initial.value("latestNull").toBool());
    }
    void recordExpanded(QQuickWindow *window,bool external,bool grant) {
        if(!enabled)return;
        title=missingRequestUiTitle(window,messageId);QVERIFY(!title.isEmpty());
        QCOMPARE(title,missingRequestUiExpectedTitle(c,grant,true,"en"));
        expandedStem="native-encrypted-"+stem(external,grant)+"-published-expanded";
        captureEncryptedUi(window,stem(external,grant),"published-expanded",c.requestId,messageId,c.metadata());
    }
    void writeFinal(QQuickWindow *window,bool external,bool grant,qulonglong historyRevision) {
        if(!enabled)return;
        QVERIFY(window);QCOMPARE(window->size(),QSize(1440,900));QCOMPARE(window->devicePixelRatio(),qreal(1));
        QCOMPARE(window->rendererInterface()->graphicsApi(),QSGRendererInterface::OpenGL);
        QVERIFY(completeTransport(c.request)&&completeTransport(c.f.cipher));
        QVERIFY(parseMessage(c.request,projectId));QVERIFY(parseMessage(c.f.cipher,projectId));
        QCOMPARE(c.request.value("user_uuid").toString(),userId);QCOMPARE(c.f.cipher.value("user_uuid").toString(),userId);
        QCOMPARE(c.request.value("uuid").toString(),c.requestId);QCOMPARE(c.f.cipher.value("uuid").toString(),messageId);
        const auto rp=c.request.value("payload").toObject(),gp=c.f.cipher.value("payload").toObject();
        QCOMPARE(rp.value("kind").toString(),QString("key_request"));
        QCOMPARE(gp.value("kind").toString(),grant?QString("key_grant"):QString("key_reject"));
        QCOMPARE(gp.value("request_message_uuid"),c.request.value("uuid"));QCOMPARE(gp.value("key_uuid"),rp.value("key_uuid"));
        QCOMPARE(gp.value("recipient_device_id"),rp.value("device_id"));
        QVERIFY(c.flush());const auto worker=c.diskBody();QVERIFY(worker);
        auto expectedWorker=c.request;expectedWorker.remove("pinned");expectedWorker.remove("updated_at");
        QVERIFY(!worker->row.contains("pinned"));QVERIFY(!worker->row.contains("updated_at"));
        QCOMPARE(worker->row.value("payload"),rp);QCOMPARE(worker->row,expectedWorker);
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.lookupCount,1);QVERIFY(c.exactQuery);QCOMPARE(c.f.posts,0);QCOMPARE(c.f.streamGets,0);QCOMPARE(c.f.bindingGets,0);
        QVERIFY(!c.f.vault.available());QVERIFY(c.f.secrets.values.isEmpty());const auto encryption=c.f.controller.encryption();
        QVERIFY(encryption.value("keyRequests").toList().isEmpty());
        const auto facts=c.metadata();QVERIFY(facts.value("recipientKnown").toBool());QCOMPARE(facts.value("recipientId").toString(),userId);
        for(const auto *field:{"ownCurrentDevice","hasKey","offerRequest","canRequest"})QVERIFY(!facts.value(field).toBool());
        QCOMPARE(c.f.controller.messages().size(),1);QVERIFY(displayed(c.f.controller,c.requestId).isEmpty());
        QCOMPARE(c.f.controller.historyCommitRevision(),historyRevision);QVERIFY(!initial.isEmpty()&&!title.isEmpty()&&!expandedStem.isEmpty());
        QCOMPARE(lookup.value("body"),QJsonValue(QJsonArray{c.request}));QCOMPARE(c.lookupRows,QJsonValue(QJsonArray{c.request}));
        QCOMPARE(actual.value("history"),QJsonValue(QJsonArray{c.f.cipher}));
        const auto topics=actual.value("/api/workspace/v1/messenger/stream_topics/"),users=actual.value("/api/workspace/v1/users/");
        QVERIFY(topics.isArray()&&users.isArray());
        const auto catalogRows=actual.value("/api/workspace/v1/messenger/streams/").toArray();QJsonObject catalog;
        for(const auto &row:catalogRows)if(row.toObject().value("uuid").toString()==streamId)catalog=row.toObject();
        QCOMPARE(catalog,c.f.stream);QVERIFY(!catalog.value("encryption").toBool());
        QVERIFY(!actual.contains("/api/workspace/v1/messenger/stream_bindings/"));
        QVERIFY(!actual.contains("/api/workspace/v1/messenger/streams/"+streamId));
        auto *header=encryptedUiItem(window,"serviceGroupToggle-"+messageId);QVERIFY(header);QVERIFY(!header->property("expanded").toBool());
        QVERIFY(!encryptedUiItem(window,"keyServiceResponse-"+messageId));
        const auto directory=qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");
        captureEncryptedUi(window,stem(external,grant),"published-collapsed",c.requestId,messageId,facts);
        const auto collapsedStem="native-encrypted-"+stem(external,grant)+"-published-collapsed";
        const QJsonObject absence{{"cryptoSession","absent"},{"vaultAvailable",false},{"keyRequestsEmpty",true},
            {"ownCurrentDevice",false},{"hasKey",false},{"offerRequest",false},{"canRequest",false},
            {"recipientKnown",true},{"recipientId",facts.value("recipientId").toString()},{"outcomeKnown",false}};
        QJsonObject snapshots{{"catalog",snapshot("/api/workspace/v1/messenger/streams/")},
            {"detail",snapshot("/api/workspace/v1/messenger/streams/"+streamId)},
            {"topics",snapshot("/api/workspace/v1/messenger/stream_topics/")},
            {"bindings",snapshot("/api/workspace/v1/messenger/stream_bindings/")},{"users",snapshot("/api/workspace/v1/users/")}};
        QJsonObject pack{{"schema","native-account-r-public-v1"},{"synthetic",true},
            {"sourceProvenance",QJsonObject{{"actualNativeCase",QString::fromLatin1(QTest::currentTestFunction())+':'+QString::fromLatin1(QTest::currentDataTag())},
                {"state","Actual passive inputs; source/binary hashes require external Root receipt"}}},
            {"ids",QJsonObject{{"project",projectId},{"user",userId},{"stream",streamId},{"topic",c.f.cipher.value("topic_uuid")},
                {"request",c.requestId},{"response",messageId}}},{"viewerName",c.f.controller.viewerDisplayName()},
            {"authorLabels",QJsonObject{{userId,facts.value("recipientName").toString()},{Issuer,facts.value("senderName").toString()}}},
            {"request",c.request},{"response",c.f.cipher},{"messages",QJsonArray{c.f.cipher}},{"catalog",catalog},{"topics",topics},
            {"bindings",QJsonValue::Null},{"users",users},{"initialState",initial},{"safeAbsenceState",absence},
            {"capture",QJsonObject{{"locale","en"},{"mode","light"},{"phase","resolved-expanded"},{"finalPhase","post-space-collapsed"},{"viewport",QJsonObject{{"width",1440},{"height",900},{"dpr",1}}},
                {"expandedFrame",expandedStem+".png"},{"collapsedFrame",collapsedStem+".png"}}},
            {"transport",QJsonObject{{"snapshots",snapshots},{"observations",observations},{"actualLookup",lookup},{"exactLookupCount",c.lookupCount}}},
            {"workerProjection",QJsonObject{{"actualBody",worker->row},{"excludedFields",QJsonArray{"pinned","updated_at"}},{"exactMatch",true}}},
            {"expected",QJsonObject{{"mainTitle",title},{"title",QJsonObject{{"en",missingRequestUiExpectedTitle(c,grant,true,"en")},
                {"ru",missingRequestUiExpectedTitle(c,grant,true,"ru")}}},{"visibleMessageCount",1},{"requestVisible",false},{"postCount",0}}}};
        const auto write=[](const QString &path,const QJsonObject &data)->bool {QFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
            const auto bytes=QJsonDocument(data).toJson(QJsonDocument::Indented);return file.write(bytes)==bytes.size();};
        const auto packPath=QDir(directory).filePath("native-"+stem(external,grant)+".public-inputs.json");QVERIFY(write(packPath,pack));
        const auto digest=[](const QString &path) {QFile file(path);if(!file.open(QIODevice::ReadOnly))return QString{};
            return QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());};
        QJsonObject receipt{{"schema","native-account-r-capture-inputs-v1"},{"publicPack",packPath},{"publicPackSha256",digest(packPath)},
            {"expandedFrameSha256",digest(QDir(directory).filePath(expandedStem+".png"))},
            {"collapsedFrameSha256",digest(QDir(directory).filePath(collapsedStem+".png"))},
            {"state","Source/binary/capture acceptance provenance is external Root-owned receipt"}};
        QVERIFY(!receipt.value("publicPackSha256").toString().isEmpty());QVERIFY(!receipt.value("expandedFrameSha256").toString().isEmpty());
        QVERIFY(!receipt.value("collapsedFrameSha256").toString().isEmpty());
        QVERIFY(write(QDir(directory).filePath("native-"+stem(external,grant)+".inputs-receipt.json"),receipt));
    }
};

#endif

// Ordinary/account setup deliberately never calls Fixture::start/useChatKeys.
// No Vault/device/issuer session is needed to establish public recipient facts.
class AccountRequestCase final {
public:
    MissingRequestCase c;
    AccountRequestCase() {
        c.f.stream["encryption"] = false;
        c.f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        c.request["author_uuid"] = userId; // another device of this account
        c.lookupRows = QJsonArray{c.request};
    }
    bool catalog() {
        auto &f = c.f;
        f.controller.login(f.tls.origin().toString(), "Synthetic", "synthetic-password", "");
        if (!QTest::qWaitFor([&f] { return f.controller.projects().size() == 1; }, 2000)) return false;
        f.controller.openProject(projectId);
        return QTest::qWaitFor([this] { return c.f.controller.streams().size() == 1 && c.cache.available(); }, 2000);
    }
    bool history() {
        auto &f = c.f; f.controller.selectStream(streamId); // real aggregate, no transient topic route
        return QTest::qWaitFor([&f] { return f.controller.topics().size() == 2
            && !f.controller.busy() && f.controller.messages().size() == 1; }, 2000);
    }
    bool start() { return catalog() && history(); }
    bool openVault() {
        auto done = std::make_shared<int>(0);
        c.f.vault.open(c.f.tls.origin(), projectId, userId, c.f.deviceName,
            [done](auto failure) { *done = failure == NativeChatKeyVault::Failure::None ? 1 : -1; });
        return QTest::qWaitFor([done] { return *done != 0; }, 2000) && *done == 1;
    }
};

#ifdef WORKSPACE_NATIVE_E2EE_UI
// Only a test-owned delivery gate: the existing real TLS/MockServer handler
// produces the canonical response once. No relay/server/duplicate serialization.
class AccountUiResponseGate final : public QObject {
    Fixture &f;
    std::function<Response(const Request &)> previous;
    std::function<void()> send;
    int matched=0;
public:
    explicit AccountUiResponseGate(Fixture &fixture,const QString &id)
        : f(fixture),previous(fixture.http.handler) {
        const QPointer<AccountUiResponseGate> weak(this);
        f.http.handler=[previous=previous,weak,id](const Request &wire) {
            auto response=previous(wire); // actual handler acknowledgment/counter
            const QUrlQuery query(wire.target);
            if(weak&&wire.method=="GET"&&wire.target.path()=="/api/workspace/v1/messenger/messages/"
                &&query.queryItemValue("uuid")==id&&query.queryItemValue("project_id")==projectId) {
                response.defer=[weak](std::function<void()> frozenSend) {
                    if(weak) {++weak->matched;if(!weak->send)weak->send=std::move(frozenSend);}
                };
            }
            return response;
        };
    }
    ~AccountUiResponseGate() override {f.http.handler=previous;send={};}
    bool available() const {return bool(previous);}
    bool acknowledgedAndHeld() const {return bool(send);}
    int matchingRequests() const {return matched;}
    bool release() {
        if(!send)return false;
        auto once=std::move(send);send={};once();return true;
    }
};
#endif

class NativeE2eeReadTest final : public QObject {
    Q_OBJECT
    QSslConfiguration previous;
    QTemporaryDir certificateDirectory;
private slots:
    void ordinaryAccountPrefilterResolvesBeforeAnyBubbleDemand_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"native-no-vault", "external-no-vault", "own-author-unresolved"}) QTest::newRow(mode) << QString(mode);
    }
    void ordinaryAccountPrefilterResolvesBeforeAnyBubbleDemand()
    {
        QFETCH(QString, mode); AccountRequestCase a; auto &c = a.c;
        c.f.stream["role"] = "guest"; c.f.issuerRole = "moderator"; // recipient inclusion is not issuer/manager policy
        if (mode == "external-no-vault") {
            c.f.stream["source_name"] = "zulip";
            c.f.stream["provider_uuid"] = "eeeeeeee-eeee-4eee-8eee-000000000010";
            c.f.stream["provider_identity"] = "synthetic-account-provider";
        }
        if (mode == "own-author-unresolved") { c.f.cipher["author_uuid"] = userId; c.f.cipher["is_own"] = true; }
        QVERIFY(a.start());
        QVERIFY(!c.f.vault.available()); QVERIFY(c.f.controller.selectedTopic().isEmpty());
        QSignalSpy changed(&c.f.controller, &NativeController::messagesChanged);
        // No resolveKeyServiceRequest call, no UI mount or getter-driven action.
        // TLS handler acknowledgment must precede every active-reply witness.
        QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QCOMPARE(displayed(c.f.controller, messageId).value("own").toBool(), mode == "own-author-unresolved");
        QTRY_VERIFY(!c.pending());
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool());
        QCOMPARE(c.metadata().value("recipientId").toString(), userId);
        QVERIFY(!c.metadata().value("ownCurrentDevice").toBool());
        QVERIFY(displayed(c.f.controller, c.requestId).isEmpty());
        QVERIFY(c.f.controller.encryption().value("keyRequests").toList().isEmpty()); // no fabricated prepared/pending session
        QVERIFY(changed.count() > 0); QVERIFY(c.exactQuery);
        for (int i = 0; i != 100; ++i) { c.f.controller.messages(); c.f.controller.encryption(); }
        QVERIFY(missingRequestTransportBarrier(c));
        QCOMPARE(c.lookupCount, 1); QCOMPARE(c.f.streamGets, 0); QCOMPARE(c.f.bindingGets, 0); QCOMPARE(c.f.posts, 0);
    }

    void ordinaryAccountPrefilterReusesOwnedBodyWithoutInventingRecipients_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"body", "indexed-history", "loaded-typed-negative"}) QTest::newRow(mode) << QString(mode);
    }
    void ordinaryAccountPrefilterReusesOwnedBodyWithoutInventingRecipients()
    {
        QFETCH(QString, mode); AccountRequestCase a; auto &c = a.c;
        const auto response = c.f.cipher;
        if (mode == "loaded-typed-negative") {
            auto negative = c.request; negative["topic_uuid"] = topicId;
            negative["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Synthetic non-request body"}};
            c.f.cipher = negative;
        }
        QVERIFY(a.catalog());
        if (mode == "body") QVERIFY(c.putBody(c.request));
        if (mode == "indexed-history") QVERIFY(c.putHistory({NativeCache::Kind::History, streamId, secondTopicId, {}}, c.request));
        QVERIFY(a.history());
        if (mode == "loaded-typed-negative") {
            QCOMPARE(displayed(c.f.controller, c.requestId).value("kind").toString(), QString("markdown"));
            QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(response)));
            QCOMPARE(displayed(c.f.controller, messageId).value("kind").toString(), QString("key_reject"));
            QVERIFY(missingRequestTransportBarrier(c));
            QCOMPARE(c.lookupCount, 0); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        } else {
            QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
            QTRY_VERIFY(c.metadata().value("recipientKnown").toBool() && c.pending()); // real disk fact before delayed HTTP
            QCOMPARE(c.metadata().value("recipientId").toString(), userId);
            QVERIFY(displayed(c.f.controller, c.requestId).isEmpty());
            QTRY_VERIFY(!c.pending()); QCOMPARE(c.lookupCount, 1);
        }
        QCOMPARE(c.f.posts, 0); QVERIFY(!c.f.vault.available());
        QVERIFY(c.f.controller.encryption().value("keyRequests").toList().isEmpty());
    }

    void ordinaryAccountPrefilterUsesOneSemanticConsumerUntilAllReferencesRetire_data()
    {
        QTest::addColumn<bool>("omitted"); QTest::newRow("transport") << false; QTest::newRow("omission") << true;
    }
    void ordinaryAccountPrefilterUsesOneSemanticConsumerUntilAllReferencesRetire()
    {
        QFETCH(bool, omitted); AccountRequestCase a; auto &c = a.c;
        c.lookupStatus = omitted ? 200 : 503; if (omitted) c.lookupRows = QJsonArray{};
        QVERIFY(a.start()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        auto second = c.f.cipher; const QString secondId = "eeeeeeee-eeee-4eee-8eee-000000000002"; second["uuid"] = secondId;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(second)));
        QTRY_VERIFY(!c.pending());
        QVERIFY(missingRequestTransportBarrier(c)); QCOMPARE(c.lookupCount, 1); // G2 is not a new R hook
        const auto remove = [&](const QJsonObject &row) {
            auto event = cryptoEvent(row); auto payload = event.value("payload").toObject(); payload["kind"] = "message.deleted";
            event["payload"] = payload; return c.f.controller.applyRealtimeEvent(event);
        };
        QVERIFY(remove(c.f.cipher)); QVERIFY(remove(second));
        QVERIFY(displayed(c.f.controller, messageId).isEmpty()); QVERIFY(displayed(c.f.controller, secondId).isEmpty());
        QVERIFY(missingRequestTransportBarrier(c)); QCOMPARE(c.lookupCount, 1);
        c.lookupStatus = 200; c.lookupRows = QJsonArray{c.request};
        auto third = second; third["uuid"] = "eeeeeeee-eeee-4eee-8eee-000000000003";
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(third)));
        QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending());
        QVERIFY(!c.metadata(third.value("uuid").toString()).value("recipientKnown").toBool());
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(c.metadata(third.value("uuid").toString()).value("recipientKnown").toBool());
        QCOMPARE(c.lookupCount, 2); QCOMPARE(c.f.posts, 0);
    }

    void ordinaryAccountForeignRequestEligibilityUsesExactPreparedGeneration_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"holder-A", "holder-B-only", "admin-without-A", "departed-requester", "completed-holder-A"}) QTest::newRow(mode) << QString(mode);
    }
    void ordinaryAccountForeignRequestEligibilityUsesExactPreparedGeneration()
    {
        QFETCH(QString, mode); AccountRequestCase a; auto &c = a.c;
        const auto response = c.f.cipher;
        c.request["author_uuid"] = SourceRecipient; c.f.cipher = c.request;
        if (mode == "admin-without-A") c.f.stream["role"] = "administrator";
        const auto roles = addSourceRecipientBinding(c.f);
        const auto previous = c.f.http.handler;
        c.f.http.handler = [&, previous](const Request &wire) {
            auto result = previous(wire);
            if (mode == "departed-requester" && wire.target.path() == "/api/workspace/v1/messenger/stream_bindings/") {
                QJsonArray rows; for (const auto &raw : result.data.toArray())
                    if (raw.toObject().value("user_uuid").toString() != SourceRecipient) rows.append(raw);
                result.data = rows;
            }
            return result;
        };
        QVERIFY(a.start()); QVERIFY(a.openVault());
        if (mode != "admin-without-A") {
            const auto key = mode == "holder-B-only" ? QString("eeeeeeee-eeee-4eee-8eee-000000000099") : returnedId;
            QCOMPARE(c.f.vault.put(c.f.vault.lease(), {streamId, key}, QByteArray(32, 'k'), 0), NativeChatKeyVault::Failure::None);
        }
        c.f.controller.useChatKeys(&c.f.vault);
        QTRY_COMPARE(c.f.streamGets, 1); QTRY_COMPARE(c.f.bindingGets, 1);
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_VERIFY(!incomingKeyRequestRow(c.f.controller, c.requestId).isEmpty());
        const auto row = incomingKeyRequestRow(c.f.controller, c.requestId);
        QCOMPARE(row.value("canManageKeys").toBool(), mode == "holder-A" || mode == "completed-holder-A" || mode == "admin-without-A");
        QCOMPARE(row.value("hasRequestedKey").toBool(), mode == "holder-A" || mode == "completed-holder-A" || mode == "departed-requester");
        QCOMPARE(roles->value(userId), c.f.stream.value("role").toString());
        if (mode == "completed-holder-A") {
            QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(response)));
            QTRY_COMPARE(incomingKeyRequestRow(c.f.controller, c.requestId).value("status").toString(), QString("rejected"));
            QVERIFY(incomingKeyRequestRow(c.f.controller, c.requestId).value("canManageKeys").toBool());
        }
        // Ordinary manager Reject is now a supported action; Grant/request remain
        // outside this slice. The Source-created R capability and actual Main
        // click tests verify the manager write. Retain every nonmanager no-write control.
        const bool managerReject = mode == "admin-without-A";
        QVERIFY(!row.value("canGrant").toBool()); QCOMPARE(row.value("canReject").toBool(), managerReject);
        c.f.controller.grantChatKey(c.requestId);
        if (!managerReject) c.f.controller.rejectChatKey(c.requestId);
        c.f.controller.requestChatKey(returnedId);
        QVERIFY(missingRequestTransportBarrier(c)); QCOMPARE(c.f.posts, 0); QCOMPARE(c.lookupCount, 0);
    }

    void ordinaryAccountPrefilterLateReplyCannotSurviveLogout()
    {
        AccountRequestCase a; auto &c = a.c; QVERIFY(a.start());
        QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        c.f.controller.logout(); QTRY_VERIFY(!c.pending());
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!c.f.controller.authenticated());
        QVERIFY(c.f.controller.messages().isEmpty()); QVERIFY(c.f.controller.encryption().value("keyRequests").toList().isEmpty());
        QCOMPARE(c.f.posts, 0); QCOMPARE(c.lookupCount, 1);
    }
    void ordinaryAccountInterruptedPublicLookupResumesAfterActualWssCatchUp()
    {
        AccountRequestCase a; auto &c = a.c;
        const auto response = c.f.cipher;
        c.f.cipher["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Synthetic pre-live history"}};
        RealtimeTransport transport(c.f); // real TLS/WSS fixture; destroyed before its Fixture
        QVERIFY(a.start()); QVERIFY(transport.start());
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(c.flush()); // settle initial snapshot/cache publications before capturing the pure-trust baseline
        QCOMPARE(c.lookupCount, 0); QVERIFY(!c.f.vault.available());
        QVERIFY(c.f.controller.selectedTopic().isEmpty());
        const auto owner = c.f.controller.ownerKey(); const auto generation = c.f.api.generation();
        const auto lease = c.cache.lease(); const auto catalogGets = c.f.catalogGets;
        const auto historyRevision = c.f.controller.historyCommitRevision();
        const auto snapshots = transport.snapshots;
        transport.publish(11, "message.updated", response);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(11));
        QCOMPARE(displayed(c.f.controller, messageId).value("kind").toString(), QString("key_reject"));
        QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending()); // actual server-acknowledged active public GET
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        const auto eventGets = transport.eventGets;
        transport.holdReady = true;
        for (auto &socket : transport.sockets) if (socket) socket->close();
        QTRY_VERIFY(c.f.controller.syncState() != "live");
        QTRY_VERIFY(!c.pending()); // checkChatLifetime physically cancels the old request
        QTRY_VERIFY(transport.eventGets > eventGets);
        QTRY_VERIFY(!transport.pendingReady.isEmpty()); // actual catch-up + held ready frame
        QCOMPARE(c.lookupCount, 1); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QCOMPARE(c.f.controller.ownerKey(), owner); QCOMPARE(c.f.api.generation(), generation);
        QCOMPARE(c.cache.lease().owner, lease.owner); QCOMPARE(c.cache.lease().generation, lease.generation);
        QCOMPARE(c.f.catalogGets, catalogGets); QCOMPARE(transport.snapshots, snapshots);
        QCOMPARE(c.f.controller.historyCommitRevision(), historyRevision); // no replacement history/catalog caused this recovery
        QCOMPARE(c.f.controller.selectedStream(), streamId); QVERIFY(c.f.controller.selectedTopic().isEmpty());
        transport.releaseReady(); QTRY_COMPARE(c.f.controller.syncState(), QString("live"));
        // No direct resolver call, disclosure/remount, sleep or synthetic signal.
        // A canceled unfinished attempt must resume; a settled Failed/Omitted
        // consumer is covered separately by the unchanged semantic-consumer rows.
        QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending());
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(c.metadata().value("recipientKnown").toBool());
        QCOMPARE(c.metadata().value("recipientId").toString(), userId);
        QVERIFY(displayed(c.f.controller, c.requestId).isEmpty());
        QVERIFY(c.f.controller.encryption().value("keyRequests").toList().isEmpty());
        QCOMPARE(c.f.streamGets, 0); QCOMPARE(c.f.bindingGets, 0); QCOMPARE(c.f.posts, 0);
        QVERIFY(c.exactQuery); QCOMPARE(c.lookupCount, 2);
    }

    // Positive observer control: ApiClient uses sendCustomRequest for GET too.
    // Real delayed TLS replies must be identified by their custom verb + scope.
    void missingRequestPendingObserverTracksActualCustomGetAndExcludesOtherRequests()
    {
        MissingRequestCase c; QVERIFY(c.start());
        QCOMPARE(c.f.streamGets, 1); QCOMPARE(c.f.bindingGets, 1);
        QVERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        const QString otherId = "eeeeeeee-eeee-4eee-8eee-000000000009";
        auto other = c.request; other["uuid"] = otherId; c.additionalRequests.insert(otherId, other);
        const auto activeGet = [&](const QString &path, const QString &id) -> QNetworkReply * {
            for (const auto reply : c.f.api.findChildren<QNetworkReply *>()) {
                if (!reply->isFinished() && reply->url().path() == path
                    && QUrlQuery(reply->url()).queryItemValue("uuid") == id
                    && reply->operation() == QNetworkAccessManager::CustomOperation
                    && reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "GET") return reply;
            }
            return nullptr;
        };
        const QString path = "/api/workspace/v1/messenger/messages/";
        const auto exactDone = std::make_shared<bool>(false);
        c.f.api.request("GET", path, {}, {{"project_id", projectId}, {"uuid", c.requestId}},
            [exactDone](const ApiResult &result) { *exactDone = result.ok(); });
        QTRY_COMPARE(c.lookupCount, 1); // TLS handler dispatch, not merely QNetworkReply allocation
        QTRY_VERIFY(activeGet(path, c.requestId));
        QVERIFY(c.pending());
        QTRY_VERIFY(*exactDone); QVERIFY(!c.pending());

        const auto otherDone = std::make_shared<bool>(false);
        c.f.api.request("GET", path, {}, {{"project_id", projectId}, {"uuid", otherId}},
            [otherDone](const ApiResult &result) { *otherDone = result.ok(); });
        QTRY_VERIFY(activeGet(path, otherId));
        QVERIFY(!c.pending()); // same path and real GET, different UUID
        QTRY_VERIFY(*otherDone); QCOMPARE(c.lookupCount, 2);

        const auto unrelatedDone = std::make_shared<bool>(false);
        c.f.api.request("GET", "/resolver-unrelated", {}, {},
            [unrelatedDone](const ApiResult &result) { *unrelatedDone = result.ok(); });
        QTRY_VERIFY(activeGet("/resolver-unrelated", QString{}));
        QVERIFY(!c.pending()); // real GET, unrelated path
        QTRY_VERIFY(*unrelatedDone);
        QVERIFY(!c.pending()); QVERIFY(c.exactQuery); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestSharedLookupSurvivesFirstResponseRetirement_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"first-deleted", "first-moved-off-topic", "both-deleted"}) QTest::newRow(mode) << QString(mode);
    }
    void missingRequestSharedLookupSurvivesFirstResponseRetirement()
    {
        QFETCH(QString, mode); MissingRequestCase c; QVERIFY(c.start());
        auto second = c.f.cipher; const QString secondId = "eeeeeeee-eeee-4eee-8eee-000000000002"; second["uuid"] = secondId;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(second)));
        QCOMPARE(displayed(c.f.controller, secondId).value("kind").toString(), QString("key_reject"));
        QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        QVERIFY(c.demand(secondId)); QCOMPARE(c.lookupCount, 1); // both current consumers share the actual delayed R GET
        const auto remove = [&](const QJsonObject &row) {
            auto event = cryptoEvent(row); auto payload = event.value("payload").toObject();
            payload["kind"] = "message.deleted"; event["payload"] = payload;
            return c.f.controller.applyRealtimeEvent(event);
        };
        if (mode == "first-moved-off-topic") {
            auto moved = c.f.cipher; moved["topic_uuid"] = secondTopicId;
            QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(moved)));
        } else QVERIFY(remove(c.f.cipher));
        QVERIFY(displayed(c.f.controller, messageId).isEmpty());
        if (mode == "both-deleted") {
            QVERIFY(remove(second)); QVERIFY(displayed(c.f.controller, secondId).isEmpty());
            QTRY_VERIFY(!c.pending()); QVERIFY(c.flush()); QVERIFY(!c.diskBody());
        } else {
            QVERIFY(!displayed(c.f.controller, secondId).isEmpty());
            QVERIFY(c.demand(secondId)); // same mounted surviving consumer; no fresh disclosure or second GET
            QTRY_VERIFY(c.metadata(secondId).value("recipientKnown").toBool());
            QTRY_VERIFY(!c.pending());
            QCOMPARE(c.metadata(secondId).value("recipientId").toString(), SourceRecipient);
            QVERIFY(c.flush()); const auto saved = c.diskBody(); QVERIFY(saved); QCOMPARE(saved->row, c.request);
        }
        QCOMPARE(c.lookupCount, 1); QVERIFY(c.exactQuery); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestNewSemanticDisclosureRetriesOnlyTransportFailure_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("HTTP503") << 503;
        QTest::newRow("HTTP404-not-omission") << 404;
        QTest::newRow("non-array200") << 200;
    }
    void missingRequestNewSemanticDisclosureRetriesOnlyTransportFailure()
    {
        QFETCH(int, status); MissingRequestCase c; QVERIFY(c.start());
        c.lookupStatus = status; c.lookupRows = QJsonObject{};
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount, 1);
        QTRY_VERIFY(!c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(discloseMissingRequest(c, 1)); QVERIFY(discloseMissingRequest(c, 0)); QVERIFY(c.demand());
        for (int n = 0; n < 20; ++n) { (void)c.f.controller.messages(); (void)c.f.controller.encryption(); }
        QVERIFY(missingRequestTransportBarrier(c)); QCOMPARE(c.lookupCount, 1);
        c.lookupStatus = 200; c.lookupRows = QJsonArray{c.request};
        QVERIFY(discloseMissingRequest(c, 2)); QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount, 2);
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool()); QTRY_VERIFY(!c.pending());
        QCOMPARE(c.metadata().value("recipientId").toString(), SourceRecipient);
        QVERIFY(c.flush()); QVERIFY(c.diskBody()); QCOMPARE(c.f.posts, 0); QVERIFY(c.exactQuery);
    }

    void missingRequestSharedRConsumesEveryActiveDisclosureWithoutDuplicateGet()
    {
        MissingRequestCase c; QVERIFY(c.start()); c.lookupStatus = 503; c.lookupRows = QJsonObject{};
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        auto second = c.f.cipher; const QString secondId = "eeeeeeee-eeee-4eee-8eee-000000000002"; second["uuid"] = secondId;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(second)));
        QVERIFY(discloseMissingRequest(c, 2, secondId)); QCOMPARE(c.lookupCount, 1);
        QTRY_VERIFY(!c.pending());
        QVERIFY(discloseMissingRequest(c, 1)); QVERIFY(discloseMissingRequest(c, 2, secondId)); QVERIFY(c.demand(secondId));
        QVERIFY(missingRequestTransportBarrier(c)); QCOMPARE(c.lookupCount, 1);
        c.lookupStatus = 200; c.lookupRows = QJsonArray{c.request};
        QVERIFY(discloseMissingRequest(c, 3, secondId)); QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount, 2);
        QVERIFY(discloseMissingRequest(c, 4)); QCOMPARE(c.lookupCount, 2); // still one active R lookup
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool());
        QTRY_VERIFY(c.metadata(secondId).value("recipientKnown").toBool()); QTRY_VERIFY(!c.pending());
        QCOMPARE(c.lookupCount, 2); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestNewDisclosureCannotRefetchAuthoritativeNegative_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"typed-other-kind", "wrong-viewer"}) QTest::newRow(mode) << QString(mode);
    }
    void missingRequestNewDisclosureCannotRefetchAuthoritativeNegative()
    {
        QFETCH(QString, mode); MissingRequestCase c; QVERIFY(c.start());
        auto row = c.request;
        if (mode == "typed-other-kind") row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Known canonical negative"}};
        else row["user_uuid"] = Issuer;
        c.lookupRows = QJsonArray{row};
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending()); QTRY_VERIFY(!c.pending());
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        c.lookupRows = QJsonArray{c.request}; // a newer disclosure is not permission to overwrite a sealed fact
        QVERIFY(discloseMissingRequest(c, 2)); QVERIFY(discloseMissingRequest(c, 50));
        QVERIFY(missingRequestTransportBarrier(c)); QCOMPARE(c.lookupCount, 1);
        QVERIFY(!c.metadata().value("recipientKnown").toBool()); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestUsefulCachedFactDoesNotRefetchOnNewDisclosureAfterRefreshError()
    {
        MissingRequestCase c; QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        c.lookupStatus = 503; c.lookupRows = QJsonObject{};
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        QVERIFY(c.metadata().value("recipientKnown").toBool()); QTRY_VERIFY(!c.pending());
        QVERIFY(discloseMissingRequest(c, 2)); QVERIFY(missingRequestTransportBarrier(c));
        QCOMPARE(c.lookupCount, 1); QVERIFY(c.metadata().value("recipientKnown").toBool());
        QVERIFY(c.flush()); QVERIFY(c.diskBody()); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestNewDisclosureRetriesFailedHttpWhileCacheIsStillOpening()
    {
        MissingRequestCase c; QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        c.holdHit(c.requestId); const auto release = qScopeGuard([&] { c.gateRelease.release(); });
        QTRY_VERIFY(c.clockEntered.load()); QVERIFY(c.cache.setDiskLimitBytes(2000000000LL));
        QVERIFY(c.cache.maintenanceBusy()); QVERIFY(!c.cache.available());
        c.lookupStatus = 503; c.lookupRows = QJsonObject{};
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending()); QTRY_VERIFY(!c.pending());
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(discloseMissingRequest(c, 1)); QCOMPARE(c.lookupCount, 1);
        c.lookupStatus = 200; c.lookupRows = QJsonArray{c.request};
        QVERIFY(discloseMissingRequest(c, 2)); QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount, 2);
        c.gateRelease.release(); QTRY_VERIFY(c.cache.available());
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool()); QTRY_VERIFY(!c.pending());
        QCOMPARE(c.lookupCount, 2); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestDeletionStillDominatesAUserDisclosedRetry()
    {
        MissingRequestCase c; QVERIFY(c.start()); c.lookupStatus = 503; c.lookupRows = QJsonObject{};
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending()); QTRY_VERIFY(!c.pending());
        c.lookupStatus = 200; c.lookupRows = QJsonArray{c.request};
        QVERIFY(discloseMissingRequest(c, 2)); QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending());
        auto event = cryptoEvent(c.request); auto payload = event.value("payload").toObject();
        payload["kind"] = "message.deleted"; event["payload"] = payload;
        QVERIFY(c.f.controller.applyRealtimeEvent(event)); QTRY_VERIFY(!c.pending());
        QVERIFY(discloseMissingRequest(c, 2)); QVERIFY(missingRequestTransportBarrier(c));
        QCOMPARE(c.lookupCount, 2); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(c.flush()); QVERIFY(!c.diskBody()); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestOmissionNewDisclosureNeedsFreshHttpAndCannotHydrateAPreOmissionPut()
    {
        MissingRequestCase c; QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        auto held = c.request; held["uuid"] = returnedId; QVERIFY(c.putBody(held));
        c.lookupRows = QJsonArray{};
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        QVERIFY(c.metadata().value("recipientKnown").toBool());
        c.holdHit(returnedId); const auto release = qScopeGuard([&] { c.gateRelease.release(); });
        QTRY_VERIFY(c.clockEntered.load());
        const auto oldPut = std::make_shared<int>(0);
        QVERIFY(c.cache.putMessageBody(c.cache.lease(), c.request, &c.f.controller,
            [oldPut](bool okay) { *oldPut = okay ? 1 : -1; }));
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(!c.metadata().value("recipientKnown").toBool());
        QTRY_COMPARE(*oldPut, -1); // actual queued pre-omission put was revoked by the required barrier

        c.lookupStatus = 503; c.lookupRows = QJsonObject{};
        QVERIFY(discloseMissingRequest(c, 2)); QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending());
        QVERIFY(!c.metadata().value("recipientKnown").toBool()); // no saved-body hydration while worker is held
        QTRY_VERIFY(!c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(discloseMissingRequest(c, 2)); QCOMPARE(c.lookupCount, 2); // same mounted failed refresh is idempotent

        auto fresh = c.request; auto freshPayload = fresh.value("payload").toObject();
        freshPayload["device_name"] = "Fresh canonical after omission"; fresh["payload"] = freshPayload;
        c.lookupStatus = 200; c.lookupRows = QJsonArray{fresh};
        QVERIFY(discloseMissingRequest(c, 3)); QTRY_COMPARE(c.lookupCount, 3); QTRY_VERIFY(c.pending());
        c.gateRelease.release(); QVERIFY(c.flush()); QVERIFY(!c.diskBody());
        QVERIFY(c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool()); QTRY_VERIFY(!c.pending());
        QCOMPARE(c.metadata().value("deviceName").toString(), QString("Fresh canonical after omission"));
        QVERIFY(c.flush()); const auto body = c.diskBody(); QVERIFY(body); QCOMPARE(body->row, fresh);
        QCOMPARE(c.lookupCount, 3); QCOMPARE(c.f.posts, 0); QVERIFY(c.exactQuery);
    }

    void missingRequestPreOmissionRetiredHttpCannotPublishIntoFreshRemountCapture()
    {
        MissingRequestCase c; QVERIFY(c.start()); c.lookupDelay = 2400;
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        QPointer<QNetworkReply> retired;
        for (const auto reply : c.f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && QUrlQuery(reply->url()).queryItemValue("uuid") == c.requestId) retired = reply;
        QVERIFY(retired);
        c.f.controller.selectTopic({}); QTRY_VERIFY(retired.isNull()); // owned aggregate route; fixture DTO remains in scope
        c.f.controller.selectTopic(topicId);
        QTRY_COMPARE(c.f.controller.selectedTopic(), topicId);
        QTRY_VERIFY(!displayed(c.f.controller, messageId).isEmpty());
        c.lookupRows = QJsonArray{}; c.lookupDelay = 1200;
        QVERIFY(discloseMissingRequest(c, 2)); QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending());
        QTRY_VERIFY(!c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(c.flush()); QVERIFY(!c.diskBody());
        const auto stalePublication = std::make_shared<bool>(false);
        const auto connection = connect(&c.f.controller, &NativeController::messagesChanged, this, [&c, stalePublication] {
            if (c.metadata().value("recipientKnown").toBool()
                && c.metadata().value("deviceName").toString() == "Resolver recipient device") *stalePublication = true;
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        auto fresh = c.request; auto freshPayload = fresh.value("payload").toObject();
        freshPayload["device_name"] = "Fresh capture after omission"; fresh["payload"] = freshPayload;
        c.lookupRows = QJsonArray{fresh};
        QVERIFY(discloseMissingRequest(c, 3)); QTRY_COMPARE(c.lookupCount, 3); QTRY_VERIFY(c.pending());
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool()); QTRY_VERIFY(!c.pending());
        QVERIFY(missingRequestTransportBarrier(c)); // passes the original delayed server-response deadline without sleeps
        QVERIFY(!*stalePublication);
        QCOMPARE(c.metadata().value("deviceName").toString(), QString("Fresh capture after omission"));
        QVERIFY(c.flush()); const auto body = c.diskBody(); QVERIFY(body); QCOMPARE(body->row, fresh);
        QCOMPARE(c.lookupCount, 3); QCOMPARE(c.f.posts, 0); QVERIFY(c.exactQuery);
    }

    // Separate OPEN Source body-resolver parity, not a change to old one-arg RT oracles.
    void missingRequestNewDisclosureAfterLiveDeleteUsesCurrentCapture_data()
    {
        QTest::addColumn<bool>("failedFreshFirst");
        QTest::newRow("fresh-canonical-after-live-delete") << false;
        QTest::newRow("failed-fresh-read-cannot-hydrate-deleted-cache") << true;
    }
    void missingRequestNewDisclosureAfterLiveDeleteUsesCurrentCapture()
    {
        QFETCH(bool, failedFreshFirst); MissingRequestCase c; QVERIFY(c.start());
        QVERIFY(discloseMissingRequest(c, 1)); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        auto event = cryptoEvent(c.request); auto payload = event.value("payload").toObject();
        payload["kind"] = "message.deleted"; event["payload"] = payload;
        QVERIFY(c.f.controller.applyRealtimeEvent(event)); QTRY_VERIFY(!c.pending());
        QVERIFY(c.demand()); QVERIFY(discloseMissingRequest(c, 1));
        QVERIFY(missingRequestTransportBarrier(c)); QCOMPARE(c.lookupCount, 1);
        QVERIFY(!c.metadata().value("recipientKnown").toBool()); QVERIFY(!c.diskBody());
        if (failedFreshFirst) {
            c.lookupStatus = 503; c.lookupRows = QJsonObject{};
            QVERIFY(discloseMissingRequest(c, 2)); QTRY_COMPARE(c.lookupCount, 2); QTRY_VERIFY(c.pending());
            QTRY_VERIFY(!c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool());
            QVERIFY(!c.diskBody());
        }
        auto fresh = c.request; auto freshPayload = fresh.value("payload").toObject();
        freshPayload["device_name"] = "Fresh canonical after live deletion"; fresh["payload"] = freshPayload;
        c.lookupStatus = 200; c.lookupRows = QJsonArray{fresh};
        QVERIFY(discloseMissingRequest(c, failedFreshFirst ? 3 : 2));
        QTRY_COMPARE(c.lookupCount, failedFreshFirst ? 3 : 2); QTRY_VERIFY(c.pending());
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool()); QTRY_VERIFY(!c.pending());
        QCOMPARE(c.metadata().value("deviceName").toString(), QString("Fresh canonical after live deletion"));
        QVERIFY(c.flush()); const auto body = c.diskBody(); QVERIFY(body); QCOMPARE(body->row, fresh);
        QCOMPARE(c.f.posts, 0); QVERIFY(c.exactQuery);
    }

    // Planned missing-capability rows: reflection keeps baseline RED executable.
    void missingRequestResolvesOnlyOnDemandAndCoalescesSameRWithoutVisibleInsertion()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        QVERIFY(!c.metadata().value("recipientKnown").toBool()); QCOMPARE(c.lookupCount, 0);
        const auto notified = std::make_shared<bool>(false);
        const auto notification = connect(&c.f.controller, &NativeController::messagesChanged, this, [&c, notified] {
            if (c.metadata().value("recipientKnown").toBool()) *notified = true;
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(notification); });
        for (int n = 0; n < 20; ++n) { (void)c.f.controller.messages(); (void)c.f.controller.encryption(); }
        QCOMPARE(c.lookupCount, 0); // loaded/collapsed data and getters are not demand
        const auto details = c.f.streamGets, bindings = c.f.bindingGets, topics = c.f.topicGets;
        QVERIFY(c.demand()); QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount, 1);
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        auto second = c.f.cipher; second["uuid"] = "eeeeeeee-eeee-4eee-8eee-000000000002";
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(second)));
        QVERIFY(c.demand(second.value("uuid").toString())); QCOMPARE(c.lookupCount, 1);
        QTRY_VERIFY(*notified); // actual writer publication, not polling to force metadata work
        QVERIFY(c.metadata().value("recipientKnown").toBool());
        QCOMPARE(c.metadata().value("recipientId").toString(), SourceRecipient);
        QCOMPARE(c.metadata().value("deviceName").toString(), QString("Resolver recipient device"));
        QTRY_VERIFY(!c.pending()); QVERIFY(c.flush());
        const auto saved = c.diskBody(); QVERIFY(saved); QCOMPARE(saved->row, c.request);
        QVERIFY(displayed(c.f.controller, c.requestId).isEmpty()); QCOMPARE(c.f.controller.messages().size(), 2);
        QVERIFY(c.exactQuery); QCOMPARE(c.f.posts, 0); QCOMPARE(c.f.streamGets, details); QCOMPARE(c.f.bindingGets, bindings); QCOMPARE(c.f.topicGets, topics);
        QVERIFY(c.demand()); QCOMPARE(c.lookupCount, 1);
    }

    void missingRequestCacheFirstUsesBodyOrIndexedHistoryThenRefreshes_data()
    {
        QTest::addColumn<bool>("history"); QTest::newRow("exact-body") << false; QTest::newRow("indexed-history") << true;
    }
    void missingRequestCacheFirstUsesBodyOrIndexedHistoryThenRefreshes()
    {
        QFETCH(bool, history); MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        const NativeCache::Key savedHistory{NativeCache::Kind::History, streamId, secondTopicId, "older-request-page"};
        if (history) QVERIFY(c.putHistory(savedHistory, c.request)); else QVERIFY(c.putBody(c.request));
        QVERIFY(c.flush()); QVERIFY(c.demand()); QTRY_VERIFY(c.pending());
        QVERIFY(c.metadata().value("recipientKnown").toBool()); // actual body publication precedes refresh reply
        QCOMPARE(c.metadata().value("recipientId").toString(), SourceRecipient);
        QVERIFY(displayed(c.f.controller, c.requestId).isEmpty());
        QTRY_VERIFY(!c.pending()); QCOMPARE(c.lookupCount, 1); QVERIFY(c.exactQuery); QCOMPARE(c.f.posts, 0);
        QVERIFY(c.flush()); const auto saved = c.diskBody(); QVERIFY(saved); QCOMPARE(saved->row, c.request);
    }

    void missingRequestLegacyMissingViewerIsCacheMissBeforeOwnedHttp()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        auto legacy = c.request; legacy.remove("user_uuid");
        QVERIFY(c.putHistory({NativeCache::Kind::History, streamId, secondTopicId, "legacy"}, legacy));
        QVERIFY(!c.diskBody()); QVERIFY(c.demand()); QTRY_VERIFY(c.pending());
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool()); QCOMPARE(c.lookupCount, 1); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestLoadedPositiveAndNegativeFactsDoNotFetch_data()
    {
        QTest::addColumn<bool>("negative"); QTest::newRow("owned-R") << false; QTest::newRow("known-full-other-kind") << true;
    }
    void missingRequestLoadedPositiveAndNegativeFactsDoNotFetch()
    {
        QFETCH(bool, negative); MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        auto known = c.request;
        if (negative) known["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Current negative control"}};
        // Same-stream off-topic full RT is a known fact, not visible window membership.
        if (negative) {
            QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.request)));
            QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(known)));
        } else QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(known)));
        QVERIFY(c.demand()); QCOMPARE(c.lookupCount, 0);
        QCOMPARE(c.metadata().value("recipientKnown").toBool(), !negative); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestOwnedMalformedOrOtherRouteCannotInventRecipient_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"wrong-viewer", "wrong-project", "wrong-stream", "wrong-kind"}) QTest::newRow(mode) << QString(mode);
    }
    void missingRequestOwnedMalformedOrOtherRouteCannotInventRecipient()
    {
        QFETCH(QString, mode); MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start()); auto row = c.request;
        if (mode == "wrong-viewer") row["user_uuid"] = Issuer;
        if (mode == "wrong-project") row["project_id"] = Issuer;
        if (mode == "wrong-stream") row["stream_uuid"] = Issuer;
        if (mode == "wrong-kind") row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Not a request"}};
        c.lookupRows = QJsonArray{row}; QVERIFY(c.demand()); QTRY_VERIFY(c.pending());
        QTRY_VERIFY(!c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(!c.metadata().value("canRequest").toBool()); QCOMPARE(c.f.posts, 0);
        QVERIFY(c.demand()); QCOMPARE(c.lookupCount, 1); // failure/known negative is not a render refetch loop
    }

    void missingRequestAuthoritativeOmissionErasesBodyAndAllHistoryCopies()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        const NativeCache::Key topic{NativeCache::Kind::History, streamId, secondTopicId, "omission-topic"};
        const NativeCache::Key aggregate{NativeCache::Kind::History, streamId, {}, "omission-aggregate"};
        QVERIFY(c.putHistory(topic, c.request)); QVERIFY(c.putHistory(aggregate, c.request));
        c.lookupRows = QJsonArray{}; QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        QVERIFY(c.metadata().value("recipientKnown").toBool());
        QVERIFY(c.putBody(c.request)); // accepted pre-omission snapshot must not survive
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(!c.metadata().value("recipientKnown").toBool()); QVERIFY(c.flush());
        QVERIFY(!c.diskBody());
        const auto inspected = std::make_shared<int>(0);
        c.cache.get(c.cache.lease(), topic, &c.f.controller, [inspected](auto page) { *inspected = page ? -1 : 1; });
        QTRY_COMPARE(*inspected, 1);
        *inspected = 0; c.cache.get(c.cache.lease(), aggregate, &c.f.controller, [inspected](auto page) { *inspected = page ? -1 : 1; });
        QTRY_COMPARE(*inspected, 1); QVERIFY(c.demand()); QCOMPARE(c.lookupCount, 1); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestFailedRefreshPreservesUsefulDiskBody_data()
    {
        QTest::addColumn<int>("status"); QTest::newRow("HTTP404-is-not-omission") << 404; QTest::newRow("HTTP503-is-not-omission") << 503;
    }
    void missingRequestFailedRefreshPreservesUsefulDiskBody()
    {
        QFETCH(int, status); MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        c.lookupStatus = status; c.lookupRows = QJsonObject{};
        QVERIFY(c.demand()); QTRY_VERIFY(c.pending()); QVERIFY(c.metadata().value("recipientKnown").toBool());
        QTRY_VERIFY(!c.pending()); QVERIFY(c.flush()); const auto saved = c.diskBody(); QVERIFY(saved); QCOMPARE(saved->row, c.request);
        QVERIFY(c.demand()); QCOMPARE(c.lookupCount, 1); QCOMPARE(c.f.posts, 0); // no permission/POST inferred
    }

    void missingRequestLateHttpCannotResurrectDeletedOrMovedRequest_data()
    {
        QTest::addColumn<bool>("deletion"); QTest::newRow("canonical-delete") << true; QTest::newRow("canonical-move-out") << false;
    }
    void missingRequestLateHttpCannotResurrectDeletedOrMovedRequest()
    {
        QFETCH(bool, deletion); MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        if (deletion) {
            auto event = cryptoEvent(QJsonObject{{"uuid", c.requestId}, {"project_id", projectId}, {"user_uuid", userId},
                {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}});
            auto payload = event.value("payload").toObject(); payload["kind"] = "message.deleted"; event["payload"] = payload;
            QVERIFY(c.f.controller.applyRealtimeEvent(event));
        } else {
            auto moved = c.request; moved["stream_uuid"] = Issuer;
            QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(moved)));
        }
        QTRY_VERIFY(!c.pending()); QVERIFY(c.flush());
        QVERIFY(!c.metadata().value("recipientKnown").toBool()); QVERIFY(!c.diskBody());
        QVERIFY(displayed(c.f.controller, c.requestId).isEmpty()); QCOMPARE(c.lookupCount, 1); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestRouteCancellationDoesNotAbortUnrelatedTransport()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start()); QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        const auto other = std::make_shared<bool>(false);
        c.f.api.request("GET", "/resolver-unrelated", {}, {}, [other](const ApiResult &result) { *other = result.ok(); });
        const auto unrelatedPending = [&] {
            const auto replies = c.f.api.findChildren<QNetworkReply *>();
            return std::any_of(replies.cbegin(), replies.cend(),
                [](QNetworkReply *reply) { return !reply->isFinished() && reply->url().path() == "/resolver-unrelated"; });
        };
        QTRY_VERIFY(unrelatedPending()); c.f.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!c.pending()); QVERIFY(unrelatedPending()); QTRY_VERIFY(*other);
        QVERIFY(c.flush()); QVERIFY(!c.diskBody()); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestCacheOpeningAfterFailedHttpHydratesWithoutSecondGet()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        c.holdHit(c.requestId); const auto release = qScopeGuard([&] { c.gateRelease.release(); });
        QTRY_VERIFY(c.clockEntered.load());
        QVERIFY(c.cache.setDiskLimitBytes(2000000000LL));
        QVERIFY(c.cache.maintenanceBusy()); QVERIFY(!c.cache.available());
        c.lookupStatus = 503; c.lookupRows = QJsonObject{}; c.lookupDelay = 0;
        QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(!c.pending());
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        c.gateRelease.release(); QTRY_VERIFY(c.cache.available());
        QTRY_VERIFY(c.metadata().value("recipientKnown").toBool());
        QCOMPARE(c.metadata().value("recipientId").toString(), SourceRecipient);
        QCOMPARE(c.lookupCount, 1); QVERIFY(displayed(c.f.controller, c.requestId).isEmpty()); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestOmissionAdmissionFailureKeepsBarrierAndReportsCacheError()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        auto other = c.request; other["uuid"] = returnedId; QVERIFY(c.putBody(other));
        c.lookupRows = QJsonArray{};
        QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending()); QVERIFY(c.metadata().value("recipientKnown").toBool());
        c.holdHit(returnedId); const auto release = qScopeGuard([&] { c.gateRelease.release(); });
        QTRY_VERIFY(c.clockEntered.load());
        for (int n = 0; n < 64; ++n) {
            const auto owner = QUuid::createUuidV5(QUuid(topicId), "resolver-cleanup-owner-" + QByteArray::number(n)).toString(QUuid::WithoutBraces);
            QVERIFY(c.cache.forgetAccount(c.f.tls.origin(), projectId, owner));
        }
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(!c.cache.error().isEmpty()); QCOMPARE(c.f.controller.encryption().value("error").toString(), c.cache.error());
        QVERIFY(c.demand()); QCOMPARE(c.lookupCount, 1); // same-lifetime tombstone blocks a stale fallback
        c.gateRelease.release(); QVERIFY(c.flush());
        QVERIFY(c.diskBody()); // rejected required erase was NOT durable; we never claim otherwise
        QVERIFY(!c.metadata().value("recipientKnown").toBool()); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestOwnerApiAndCacheReplacementCancelLatePublications_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"logout", "api-generation", "cache-replacement"}) QTest::newRow(mode) << QString(mode);
    }
    void missingRequestOwnerApiAndCacheReplacementCancelLatePublications()
    {
        QFETCH(QString, mode); MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        QVERIFY(c.demand()); QTRY_COMPARE(c.lookupCount, 1); QTRY_VERIFY(c.pending());
        if (mode == "logout") c.f.controller.logout();
        else if (mode == "api-generation") c.f.api.setAccessToken("synthetic-new-resolver-token");
        else c.f.controller.useCache(nullptr);
        QTRY_VERIFY(!c.pending());
        QVERIFY(c.flush());
        NativeCache inspect(c.cacheDirectory.path()); QVERIFY(inspect.open(c.f.tls.origin(), projectId, userId));
        QVERIFY(!inspect.getMessageBody(inspect.lease(), c.requestId));
        QCOMPARE(c.lookupCount, 1); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestFiveDistinctDemandsUseAtMostFourActualHttpReplies()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        QStringList responses{messageId};
        for (int n = 1; n < 5; ++n) {
            auto request = c.request; const auto id = QUuid::createUuidV5(QUuid(topicId), "resolver-r-" + QByteArray::number(n)).toString(QUuid::WithoutBraces);
            request["uuid"] = id; c.additionalRequests.insert(id, request);
            auto response = c.f.cipher; const auto responseId = QUuid::createUuidV5(QUuid(topicId), "resolver-response-" + QByteArray::number(n)).toString(QUuid::WithoutBraces);
            response["uuid"] = responseId; auto payload = response.value("payload").toObject(); payload["request_message_uuid"] = id; response["payload"] = payload;
            QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(response))); responses.append(responseId);
        }
        for (const auto &id : responses) QVERIFY(c.demand(id));
        QTRY_COMPARE(c.lookupCount, 4); QCOMPARE(c.maxActive, 4);
        const auto allKnown = [&] { return std::all_of(responses.cbegin(), responses.cend(), [&](const QString &id) {
            return c.metadata(id).value("recipientKnown").toBool();
        }); };
        QTRY_VERIFY(allKnown()); QCOMPARE(c.lookupCount, 5); QVERIFY(c.maxActive <= 4); QVERIFY(c.exactQuery); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestForeignOwnerDiskBodyCannotHydrateCurrentViewer()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start());
        auto otherViewer = c.request; otherViewer["user_uuid"] = Issuer;
        {
            NativeCache other(c.cacheDirectory.path()); QVERIFY(other.open(c.f.tls.origin(), projectId, Issuer));
            QVERIFY(other.putMessageBody(other.lease(), otherViewer)); QVERIFY(other.getMessageBody(other.lease(), c.requestId));
        }
        c.lookupStatus = 503; c.lookupRows = QJsonObject{};
        QVERIFY(c.demand()); QTRY_VERIFY(c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QTRY_VERIFY(!c.pending()); QVERIFY(!c.metadata().value("recipientKnown").toBool()); QVERIFY(!c.diskBody());
        NativeCache other(c.cacheDirectory.path()); QVERIFY(other.open(c.f.tls.origin(), projectId, Issuer));
        QVERIFY(other.getMessageBody(other.lease(), c.requestId)); QCOMPARE(c.lookupCount, 1); QCOMPARE(c.f.posts, 0);
    }

    void missingRequestFixtureTeardownReapsTheActiveReply()
    {
        QPointer<QNetworkReply> pending;
        {
            auto c = std::make_unique<MissingRequestCase>(); QVERIFY(c->hasApi()); QVERIFY(c->start());
            QVERIFY(c->demand()); QTRY_VERIFY(c->pending());
            for (const auto reply : c->f.api.findChildren<QNetworkReply *>())
                if (!reply->isFinished() && QUrlQuery(reply->url()).queryItemValue("uuid") == c->requestId) pending = reply;
            QVERIFY(pending);
            c.reset();
        }
        QTRY_VERIFY(pending.isNull()); // whole real Fixture lifetime, not an isolated Controller-only proof
    }

    void missingRequestCachedPublicationReentrantNavigationPreventsOldRefresh()
    {
        MissingRequestCase c; QVERIFY(c.hasApi()); QVERIFY(c.start()); QVERIFY(c.putBody(c.request));
        const auto changed = std::make_shared<bool>(false);
        const auto connection = connect(&c.f.controller, &NativeController::messagesChanged, this, [&c, changed] {
            if (!*changed && c.metadata().value("recipientKnown").toBool()) {
                *changed = true; c.f.controller.selectTopic(secondTopicId);
            }
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        QVERIFY(c.demand()); QTRY_VERIFY(*changed);
        QCOMPARE(c.f.controller.selectedTopic(), secondTopicId); QCOMPARE(c.lookupCount, 0);
        QVERIFY(c.flush()); QVERIFY(c.diskBody()); QCOMPARE(c.f.posts, 0);
    }

    void initTestCase() {
        previous = QSslConfiguration::defaultConfiguration();
        QVERIFY(certificateDirectory.isValid());
        CertificatePath = certificateDirectory.filePath("synthetic-ca.pem");
        PrivateKeyPath = certificateDirectory.filePath("synthetic-ca-key.pem");
        QProcess generate;
        generate.start(QStringLiteral(WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE), {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-subj", "/CN=cassi-native-e2ee-loopback", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
            "-addext", "basicConstraints=critical,CA:TRUE", "-keyout", PrivateKeyPath, "-out", CertificatePath});
        QVERIFY(generate.waitForFinished(10000)); QCOMPARE(generate.exitCode(), 0);
        QVERIFY(QFile::setPermissions(PrivateKeyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QFile certificate(CertificatePath); QVERIFY(certificate.open(QIODevice::ReadOnly));
        auto trusted = previous; auto authorities = trusted.caCertificates();
        authorities.append(QSslCertificate(certificate.readAll())); trusted.setCaCertificates(authorities);
        QSslConfiguration::setDefaultConfiguration(trusted);
    }
    void cleanupTestCase() { QSslConfiguration::setDefaultConfiguration(previous); }
    void init() { QTest::failOnWarning(); }

    // Separate supplement. SOURCE-OBSERVED CONCERN, NOT AN ACTUAL RED YET.
    // Run only after the 500-row status/preparation capability is established.
    void announcementLimitPublishesNoActionsToNotificationDrivenConsumer()
    {
        Fixture f; QVERIFY(f.start());
        const auto announcement = [](int n) {
            auto dto = message({}, QUuid::createUuidV5(QUuid(streamId), ("announcement-limit-" + QString::number(n)).toUtf8()).toString(QUuid::WithoutBraces));
            dto["author_uuid"] = Issuer; dto["is_own"] = false;
            dto["payload"] = QJsonObject{{"kind", "key_announced"},
                {"key_uuid", QUuid::createUuidV5(QUuid(streamId), ("generation-limit-" + QString::number(n)).toUtf8()).toString(QUuid::WithoutBraces)}};
            return dto;
        };
        const auto actions = [&] {
            int result = 0;
            for (const auto &value : f.controller.messages())
                if (value.toMap().value("keyServiceAnnouncement").toMap().value("canRequest").toBool()) ++result;
            return result;
        };
        for (int n = 0; n < 500; ++n) QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(announcement(n))));
        QTRY_COMPARE(actions(), 500);
        QVector<int> notifications;
        const auto connection = connect(&f.controller, &NativeController::messagesChanged, this, [&] {
            notifications.append(actions()); // Getter only in the actual notification, never polled to produce a value.
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        const auto posts = f.posts, details = f.streamGets, bindings = f.bindingGets;
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(announcement(500))));
        QTRY_VERIFY(notifications.contains(0));
        QCOMPARE(f.posts, posts); QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings);
    }

    // Independent positive supplement, SOURCE-ONLY / NOT RUN.
    // Sequence after the 500-announcement capability passes. Same V3 core
    // with/without only SERVICE-GENERATION-CAP-SUPPLEMENT is the comparison.
    void retainedAnnouncementGenerationCacheCannotAdmitA501stRequestKey()
    {
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        const auto roles = addSourceRecipientBinding(f);
        QVERIFY(f.start());
        const auto generation = [](int n) {
            return QUuid::createUuidV5(QUuid(streamId), ("cap-generation-" + QString::number(n)).toUtf8()).toString(QUuid::WithoutBraces);
        };
        const auto announcement = [&](int n) {
            auto dto = message({}, QUuid::createUuidV5(QUuid(streamId), ("cap-announcement-" + QString::number(n)).toUtf8()).toString(QUuid::WithoutBraces));
            dto["author_uuid"] = Issuer; dto["is_own"] = false;
            dto["payload"] = QJsonObject{{"kind", "key_announced"}, {"key_uuid", generation(n)}};
            return dto;
        };
        QVector<QJsonObject> announcements;
        for (int n = 0; n < 500; ++n) {
            auto dto = announcement(n); QVERIFY(parseMessage(dto, projectId));
            announcements.append(dto);
            QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(dto)));
        }
        const auto preparedKeys = [&] {
            QSet<QString> result;
            for (const auto &value : f.controller.messages()) {
                const auto row = value.toMap(), fact = row.value("keyServiceAnnouncement").toMap();
                if (row.value("kind").toString() == "key_announced"
                    && row.value("serviceStatus").toString() == "action-required"
                    && fact.value("offerRequest").toBool() && fact.value("canRequest").toBool())
                    result.insert(fact.value("keyUuid").toString());
            }
            return result;
        };
        QTRY_COMPARE(preparedKeys().size(), 500); // Actual 500 distinct prepared generations, not a guessed internal size.
        const auto initialKeys = preparedKeys();
        for (int n = 0; n < 500; ++n) QVERIFY(initialKeys.contains(generation(n)));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(roles->value(userId), QString("administrator"));
        const auto owner = f.controller.ownerKey();
        const auto lease = f.vault.lease();
        const auto details = f.streamGets, bindings = f.bindingGets, topics = f.topicGets, posts = f.posts;
        QCOMPARE(posts, 0);
        for (const auto &dto : announcements) {
            auto event = cryptoEvent(QJsonObject{{"uuid", dto.value("uuid")}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"topic_uuid", topicId}, {"user_uuid", userId}});
            auto payload = event.value("payload").toObject(); payload["kind"] = "message.deleted"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
        }
        // A zero-task barrier follows the already queued existing control work;
        // no sleep or new duration is used to simulate deletion completion.
        const auto queuedDeletionWorkDrained = std::make_shared<bool>(false);
        QTimer::singleShot(0, &f.controller, [queuedDeletionWorkDrained] { *queuedDeletionWorkDrained = true; });
        QTRY_VERIFY(*queuedDeletionWorkDrained);
        QCOMPARE(f.controller.messages().size(), 1); // Only original cipher remains; active control input is below500.
        QCOMPARE(preparedKeys().size(), 0);
        for (const auto &dto : announcements) QVERIFY(displayed(f.controller, dto.value("uuid").toString()).isEmpty());
        QCOMPARE(f.controller.ownerKey(), owner); QCOMPARE(f.vault.lease().generation, lease.generation);
        QCOMPARE(f.vault.lease().owner, lease.owner); QCOMPARE(f.vault.lease().device, lease.device);
        QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings); QCOMPARE(f.topicGets, topics);
        QVERIFY(f.controller.selectedStream() == streamId && f.controller.selectedTopic() == topicId);

        SourceRecipientPeer peer;
        const auto newKey = generation(500);
        QVERIFY(!initialKeys.contains(newKey));
        QVERIFY(peer.prepare(f, false, newKey)); // Actual independent pending Source R; no Native key seed/import.
        QCOMPARE(peer.ready.value("recipientPending").toInt(), 1);
        QCOMPARE(peer.requestDto.value("payload").toObject().value("key_uuid").toString(), newKey);
        QVERIFY(parseMessage(peer.requestDto, projectId));
        QStringList notifications;
        const auto connection = connect(&f.controller, &NativeController::messagesChanged, this, [&] {
            const auto row = displayed(f.controller, peer.requestId);
            if (!row.isEmpty()) notifications.append(row.value("serviceStatus").toString());
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QCOMPARE(displayed(f.controller, peer.requestId).value("kind").toString(), QString("key_request"));
        QCOMPARE(displayed(f.controller, peer.requestId).value("authorId").toString(), SourceRecipient);
        QTRY_VERIFY(notifications.contains("unavailable"));
        QVERIFY(incomingKeyRequestRow(f.controller, peer.requestId).isEmpty()); // No501st cached-generation admission/actions.
        QCOMPARE(displayed(f.controller, peer.requestId).value("serviceStatus").toString(), QString("unavailable"));
        QCOMPARE(f.posts, posts); QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings); QCOMPARE(f.topicGets, topics);
        const auto unchanged = peer.finish(); QVERIFY(unchanged.value("ok").toBool());
        QVERIFY(unchanged.value("recipientStillPending").toBool()); QVERIFY(!unchanged.value("recipientHasKey").toBool());
    }

    // Additive private slots for NativeE2eeReadTest. SOURCE-ONLY / NOT RUN.
    void serviceRequestStatusReducesInlineActionsAndTerminalOutcomes_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"foreign-action", "own-current-inline-hidden", "pending-disabled", "terminal-granted"})
            QTest::newRow(mode) << QString(mode);
    }
    void serviceRequestStatusReducesInlineActionsAndTerminalOutcomes()
    {
        QFETCH(QString, mode); IssuerCase c; QVERIFY(c.start());
        auto &f = c.f;
        QTRY_VERIFY(c.row().value("canGrant").toBool());
        const auto status = [&] { return displayed(f.controller, c.peer.requestId).value("serviceStatus").toString(); };
        QVERIFY(displayed(f.controller, c.peer.requestId).contains("serviceStatus"));
        QCOMPARE(status(), QString("action-required"));
        const auto posts = f.posts;
        if (mode == "own-current-inline-hidden") {
            auto own = c.peer.requestDto; own["author_uuid"] = userId; own["is_own"] = true;
            auto payload = own.value("payload").toObject(); payload["device_id"] = f.vault.device().id; own["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(updatedControl(own)));
            QTRY_VERIFY(c.row().value("ownCurrentDevice").toBool());
            // Core admission is distinct from Source inline presentation.
            QTRY_VERIFY(c.row().value("canReject").toBool());
            QTRY_COMPARE(status(), QString("waiting"));
        } else if (mode == "pending-disabled" || mode == "terminal-granted") {
            if (mode == "pending-disabled") f.postDelay = 1200;
            f.controller.grantChatKey(c.peer.requestId);
            QTRY_COMPARE(f.posts, posts + 1);
            if (mode == "pending-disabled") {
                QPointer<QNetworkReply> reply;
                for (auto *item : f.api.findChildren<QNetworkReply *>())
                    if (!item->isFinished()
                        && item->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "POST"
                        && item->url().path() == "/api/workspace/v1/messenger/messages/") reply = item;
                QVERIFY(reply);
                QVERIFY(c.row().value("offerGrant").toBool());
                QVERIFY(!c.row().value("canGrant").toBool());
                QCOMPARE(status(), QString("waiting"));
                QVERIFY(reply && !reply->isFinished());
            }
            QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
            QTRY_COMPARE(c.row().value("status").toString(), QString("granted"));
            QCOMPARE(status(), QString{});
            const auto imported = c.finishActual(); QVERIFY(imported.value("ok").toBool());
            QVERIFY(imported.value("matchesExpected").toBool());
            return;
        }
        QCOMPARE(f.posts, posts);
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void serviceResponseStatusUsesExactRecipientAndNeverMarksGrantUnavailable_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"reject-own-action", "reject-own-requested", "reject-other-device", "grant-without-state"})
            QTest::newRow(mode) << QString(mode);
    }
    void serviceResponseStatusUsesExactRecipientAndNeverMarksGrantUnavailable()
    {
        QFETCH(QString, mode); Fixture f; QVERIFY(f.start());
        f.controller.requestChatKey(returnedId);
        QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        const auto canonicalRequest = f.savedRequest;
        const auto canonicalRequestId = canonicalRequest.value("uuid").toString();
        QVERIFY(!canonicalRequestId.isEmpty());
        QCOMPARE(canonicalRequest.value("topic_uuid").toString(), secondTopicId);
        QCOMPARE(canonicalRequest.value("payload").toObject().value("kind").toString(), QString("key_request"));
        QVERIFY(parseMessage(canonicalRequest, projectId));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(canonicalRequest)));
        QTRY_VERIFY(incomingKeyRequestRow(f.controller, canonicalRequestId).value("ownCurrentDevice").toBool());
        QTRY_COMPARE(incomingKeyRequestRow(f.controller, canonicalRequestId).value("status").toString(), QString("pending"));
        QVERIFY(displayed(f.controller, canonicalRequestId).isEmpty()); // Observed off-topic R is not inserted into this window.
        QCOMPARE(f.controller.selectedTopic(), topicId);
        const auto source = f.sourceResponse(mode != "grant-without-state"); QVERIFY(source.value("ok").toBool());
        auto response = f.responseDto(source);
        QCOMPARE(f.savedRequest.value("topic_uuid").toString(), secondTopicId); // Original R stays in its actual default.
        QCOMPARE(response.value("topic_uuid").toString(), secondTopicId);
        const auto sourcePayload = response.value("payload");
        response["topic_uuid"] = topicId; // Same-stream, cross-topic response must actually enter this selected model.
        QVERIFY(response.value("payload") == sourcePayload); // No payload dump on a failed assertion.
        QVERIFY(parseMessage(response, projectId));
        if (mode == "reject-other-device") {
            auto payload = response.value("payload").toObject(); payload["recipient_device_id"] = SourceRecipientDevice;
            response["payload"] = payload;
        }
        if (mode == "grant-without-state") f.controller.useChatKeys(nullptr);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(response)));
        const auto id = response.value("uuid").toString();
        const auto row = [&] { return displayed(f.controller, id); };
        QTRY_COMPARE(row().value("kind").toString(), mode == "grant-without-state" ? QString("key_grant") : QString("key_reject"));
        QTRY_VERIFY(row().contains("serviceStatus"));
        if (mode == "reject-other-device" || mode == "grant-without-state") {
            QCOMPARE(row().value("serviceStatus").toString(), QString{});
        } else {
            QTRY_COMPARE(row().value("serviceStatus").toString(), QString("action-required"));
            if (mode == "reject-own-requested") {
                f.controller.requestChatKey(returnedId);
                QTRY_COMPARE(f.posts, 2);
                QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.isEmpty());
                QTRY_COMPARE(row().value("serviceStatus").toString(), QString("waiting"));
                QVERIFY(row().value("keyServiceResponse").toMap().value("offerRequest").toBool());
                QVERIFY(!row().value("keyServiceResponse").toMap().value("canRequest").toBool());
            }
        }
        QCOMPARE(row().value("keyServiceResponse").toMap().size(), 11);
    }

    void serviceAnnouncementUsesHistoricalAvailabilityAndStableOffer_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"missing-A-current-B", "missing-A-current-null", "known-denied-issuer", "requested", "ready"})
            QTest::newRow(mode) << QString(mode);
    }
    void serviceAnnouncementUsesHistoricalAvailabilityAndStableOffer()
    {
        QFETCH(QString, mode); Fixture f;
        f.stream = fullEncryptedStream();
        if (mode == "missing-A-current-B") f.stream["current_encryption_key_uuid"] = OtherGeneration;
        if (mode == "missing-A-current-null") f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        if (mode == "known-denied-issuer") {
            f.stream["owner"] = userId; f.stream["role"] = "owner"; f.issuerRole = "member";
            addSourceRecipientBinding(f);
        }
        QVERIFY(f.start());
        if (mode == "ready") QVERIFY(f.prepareA());
        auto announcement = message({}, QUuid::createUuid().toString(QUuid::WithoutBraces));
        announcement["author_uuid"] = Issuer; announcement["is_own"] = false;
        announcement["payload"] = QJsonObject{{"kind", "key_announced"}, {"key_uuid", returnedId}};
        QVERIFY(parseMessage(announcement, projectId));
        const auto id = announcement.value("uuid").toString();
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(announcement)));
        const auto row = [&] { return displayed(f.controller, id); };
        QTRY_VERIFY(row().contains("serviceStatus"));
        QTRY_VERIFY(row().value("serviceStatus").toString() != "unavailable");
        auto facts = row().value("keyServiceAnnouncement").toMap();
        QCOMPARE(facts.size(), 3);
        QCOMPARE(facts.value("keyUuid").toString(), returnedId);
        QVERIFY(row().value("keyServiceResponse").toMap().isEmpty());
        if (mode == "ready" || mode == "known-denied-issuer") {
            QVERIFY(!facts.value("offerRequest").toBool()); QVERIFY(!facts.value("canRequest").toBool());
            QCOMPARE(row().value("serviceStatus").toString(), QString{});
        } else {
            QVERIFY(facts.value("offerRequest").toBool()); QVERIFY(facts.value("canRequest").toBool());
            QCOMPARE(row().value("serviceStatus").toString(), QString("action-required"));
            if (mode == "requested") {
                f.controller.requestChatKey(returnedId);
                QTRY_COMPARE(f.posts, 1);
                QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.isEmpty());
                QTRY_COMPARE(row().value("serviceStatus").toString(), QString("waiting"));
                facts = row().value("keyServiceAnnouncement").toMap();
                QVERIFY(facts.value("offerRequest").toBool()); QVERIFY(!facts.value("canRequest").toBool());
            }
        }
    }

    void serviceAnnouncementPublishesUnknownToKnownWithoutGetterDrivenWork()
    {
        Fixture f; QVERIFY(f.start()); f.streamDelay = 1200;
        auto announcement = message({}, QUuid::createUuid().toString(QUuid::WithoutBraces));
        announcement["author_uuid"] = Issuer; announcement["is_own"] = false;
        announcement["payload"] = QJsonObject{{"kind", "key_announced"}, {"key_uuid", OtherGeneration}};
        const auto id = announcement.value("uuid").toString();
        QStringList notifications;
        const auto observed = connect(&f.controller, &NativeController::messagesChanged, this, [&] {
            const auto row = displayed(f.controller, id);
            if (!row.isEmpty()) notifications.append(row.value("serviceStatus").toString());
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); });
        const auto details = f.streamGets, bindings = f.bindingGets, topics = f.topicGets;
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(announcement)));
        QTRY_COMPARE(f.streamGets, details + 1);
        QPointer<QNetworkReply> detail;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/streams/" + streamId) detail = reply;
        QVERIFY(detail); QVERIFY(notifications.contains("unavailable"));
        QCOMPARE(f.bindingGets, bindings);
        QTRY_VERIFY(notifications.contains("action-required"));
        QCOMPARE(f.streamGets, details + 1); QCOMPARE(f.bindingGets, bindings + 1); QCOMPARE(f.topicGets, topics + 1);
        QCOMPARE(f.posts, 0);
        const auto requests = f.http.requests.size();
        // Functional no-network control. The separate mechanical getter call-
        // graph guard checks the no-Vault-I/O property without a new product seam.
        for (int i = 0; i < 30; ++i) {
            const auto row = displayed(f.controller, id);
            QCOMPARE(row.value("serviceStatus").toString(), QString("action-required"));
            QVERIFY(row.value("keyServiceAnnouncement").toMap().value("canRequest").toBool());
        }
        QCOMPARE(f.http.requests.size(), requests);
    }

    void fiveHundredAnnouncementsStayBoundedAndYield()
    {
        Fixture f; QVERIFY(f.start());
        const auto announcement = [](int n) {
            auto dto = message({}, QUuid::createUuidV5(QUuid(streamId), ("announcement-" + QString::number(n)).toUtf8()).toString(QUuid::WithoutBraces));
            dto["author_uuid"] = Issuer; dto["is_own"] = false;
            dto["payload"] = QJsonObject{{"kind", "key_announced"},
                {"key_uuid", QUuid::createUuidV5(QUuid(streamId), ("generation-" + QString::number(n)).toUtf8()).toString(QUuid::WithoutBraces)}};
            return dto;
        };
        for (int n = 0; n < 500; ++n) QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(announcement(n))));
        const auto count = [&] {
            int ready = 0;
            for (const auto &item : f.controller.messages()) {
                const auto row = item.toMap();
                if (row.value("kind").toString() == "key_announced" && row.value("serviceStatus").toString() == "action-required") ++ready;
            }
            return ready;
        };
        QElapsedTimer elapsed; elapsed.start(); qint64 previous = 0, maximum = 0; int pulses = 0;
        QTimer pulse; pulse.setInterval(2);
        const auto connection = connect(&pulse, &QTimer::timeout, this, [&] {
            const auto now = elapsed.nsecsElapsed(); maximum = std::max(maximum, now - previous); previous = now; ++pulses;
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        pulse.start(); QTRY_COMPARE(count(), 500); pulse.stop();
        qInfo("500-announcement preparation: %lld ms total, max pulse %.3f ms, %d pulses", elapsed.elapsed(), maximum / 1000000.0, pulses);
        QVERIFY(pulses > 2); QVERIFY(maximum < 100000000); // Same meaningful 100 ms boundary, not run yet.
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        const auto network = f.http.requests.size();
        for (int n = 0; n < 10; ++n) { f.controller.messages(); f.controller.encryption(); }
        QCOMPARE(f.http.requests.size(), network);
    }

    void issuerSourceRightsAndDispatch_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"archived-grant", "closed-default-reject", "same-account-other-device", "requester-no-current-binding"})
            QTest::newRow(mode) << QString(mode);
    }
    void issuerSourceRightsAndDispatch() {
        QFETCH(QString, mode); IssuerCase c;
        bool absentBinding = false;
        const auto previous = c.f.http.handler;
        c.f.http.handler = [&](const Request &request) {
            auto reply = previous(request);
            if (mode == "closed-default-reject" && request.target.path() == "/api/workspace/v1/messenger/stream_topics/") {
                auto rows = reply.data.toArray();
                for (qsizetype i = 0; i < rows.size(); ++i) {
                    auto row = rows[i].toObject(); if (row.value("is_default").toBool()) row["is_done"] = true; rows[i] = row;
                }
                reply.data = rows;
            }
            if (mode == "requester-no-current-binding" && request.target.path() == "/api/workspace/v1/messenger/stream_bindings/") {
                QJsonArray rows;
                for (const auto &row : reply.data.toArray()) if (row.toObject().value("user_uuid").toString() != SourceRecipient) rows.append(row);
                reply.data = rows; absentBinding = true;
            }
            return reply;
        };
        const auto restore = qScopeGuard([&] { c.f.http.handler = previous; });
        if (mode == "archived-grant") c.f.stream["is_archived"] = true;
        QVERIFY(c.start(mode == "archived-grant"));
        if (mode == "same-account-other-device") {
            auto own = c.peer.requestDto; own["author_uuid"] = userId; own["is_own"] = true;
            QVERIFY(own.value("payload").toObject().value("device_id").toString() != c.f.vault.device().id);
            QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(own)));
        }
        const bool reject = mode == "closed-default-reject";
        QTRY_VERIFY(c.row().value(reject ? "canReject" : "canGrant").toBool());
        const auto posts = c.f.posts;
        if (reject) c.f.controller.rejectChatKey(c.peer.requestId); else c.f.controller.grantChatKey(c.peer.requestId);
        QTRY_COMPARE(c.response().value("phase").toString(), QString("ready")); QCOMPARE(c.f.posts, posts + 1);
        QCOMPARE(c.f.requestBody.value("topic_uuid").toString(), secondTopicId);
        QCOMPARE(c.f.requestBody.value("payload").toObject().value("request_message_uuid").toString(), c.peer.requestId);
        if (mode == "requester-no-current-binding") QVERIFY(absentBinding);
        if (mode == "same-account-other-device") {
            // Core metadata/HTTP control only; this independent helper owns a
            // different account. Do not call it a same-account peer import proof.
            QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
        } else {
            const auto completion = c.finishActual(); QVERIFY(completion.value("ok").toBool());
            QCOMPARE(completion.value("completionOutcome").toString(), reject ? QString("rejected") : QString("granted"));
            if (!reject) QVERIFY(completion.value("matchesExpected").toBool());
        }
        for (const auto &request : c.f.http.requests) QVERIFY(!request.target.path().contains("unarchive"));
    }

    void nonManagersCannotIssueEvenWithA_data() {
        QTest::addColumn<QString>("role");
        QTest::newRow("member-with-A") << QString("member"); QTest::newRow("guest-with-A") << QString("guest");
    }
    void nonManagersCannotIssueEvenWithA() {
        QFETCH(QString, role); IssuerCase c; c.f.stream["role"] = role; QVERIFY(c.start());
        auto key = c.f.vault.read(c.f.vault.lease(), {streamId, returnedId});
        const auto wipe = qScopeGuard([&] { key.value.bytes.fill(0); }); QCOMPARE(key.value.bytes.size(), 32);
        const auto before = c.f.posts;
        QVERIFY(!c.row().value("canGrant").toBool()); QVERIFY(!c.row().value("canReject").toBool());
        c.f.controller.grantChatKey(c.peer.requestId); c.f.controller.rejectChatKey(c.peer.requestId);
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(c.f.posts, before);
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void issuerCanRejectCanonicalNonX25519RequestWithoutEncrypting() {
        IssuerCase c; QVERIFY(c.start());
        auto request = c.peer.requestDto; auto payload = request.value("payload").toObject();
        payload["public_key"] = QString::fromLatin1(QByteArray(31, 'x').toBase64()); request["payload"] = payload;
        QVERIFY(parseMessage(request, projectId)); // Backend/Source decoder accept nonempty canonicalBase64, not only32bytes.
        c.f.savedRequests[c.peer.requestId] = request;
        QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(request)));
        QVERIFY(!c.row().value("canGrant").toBool());
        QTRY_VERIFY2(c.row().value("canReject").toBool(), "Source rejectKey does not encrypt or validate an X25519 public-key length");
        const auto before = c.f.posts; c.f.controller.rejectChatKey(c.peer.requestId);
        QTRY_COMPARE(c.response().value("phase").toString(), QString("ready")); QCOMPARE(c.f.posts, before + 1);
        const auto wire = c.f.requestBody.value("payload").toObject();
        QCOMPARE(wire.value("kind").toString(), QString("key_reject")); QVERIFY(!wire.contains("content"));
        const auto result = c.finishActual(); QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("completionOutcome").toString(), QString("rejected")); QVERIFY(!result.value("recipientHasKey").toBool());
    }

    void issuerDeletionBeforePostCannotUseTheCapturedKey() {
        IssuerCase c; QVERIFY(c.start()); QTRY_VERIFY(c.row().value("canGrant").toBool());
        bool removed = false; const auto before = c.f.posts;
        const auto connection = connect(&c.f.controller, &NativeController::stateChanged, this, [&] {
            if (!removed && c.response().value("phase").toString() == "sending") {
                removed = true; QCOMPARE(c.f.vault.remove(c.f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
            }
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        c.f.controller.grantChatKey(c.peer.requestId); QVERIFY(removed);
        QTRY_VERIFY(!c.response().value("busy").toBool()); QCOMPARE(c.f.posts, before);
        QVERIFY(c.f.vault.read(c.f.vault.lease(), {streamId, returnedId}).value.bytes.isEmpty());
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void issuerUncertainStatusAndFrozenRetry_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"applied-503-key-deleted-GET-only", "absent-Check-only-then-identical-Retry", "new-default-retains-owned-original-topic"})
            QTest::newRow(mode) << QString(mode);
    }
    void issuerUncertainStatusAndFrozenRetry() {
        QFETCH(QString, mode); IssuerCase c; QVERIFY(c.start()); QTRY_VERIFY(c.row().value("canGrant").toBool());
        const bool applied = mode.startsWith("applied"); UncertainIssuerPost uncertain(c.f, applied);
        const auto before = c.f.posts; c.f.controller.grantChatKey(c.peer.requestId);
        QTRY_VERIFY(uncertain.failed); QTRY_COMPARE(c.response().value("phase").toString(), QString("uncertain"));
        QCOMPARE(c.f.posts, before + 1); const auto frozen = uncertain.frozen;
        const auto publicId = uncertain.canonical.value("uuid").toString();
        const auto path = "/api/workspace/v1/messenger/messages/" + publicId;
        const auto gets = exactRequests(c.f, "GET", path);
        if (applied) {
            QCOMPARE(c.f.vault.remove(c.f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
            QVERIFY(c.response().value("canCheck").toBool()); QVERIFY(!c.response().value("canRetry").toBool());
            c.f.controller.checkChatKeyResponse(c.peer.requestId);
            QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
            QCOMPARE(exactRequests(c.f, "GET", path), gets + 1); QCOMPARE(c.f.posts, before + 1);
            QVERIFY(c.f.vault.read(c.f.vault.lease(), {streamId, returnedId}).value.bytes.isEmpty());
        } else {
            for (int check = 1; check <= 2; ++check) {
                c.f.controller.checkChatKeyResponse(c.peer.requestId);
                QTRY_COMPARE(exactRequests(c.f, "GET", path), gets + check);
                QTRY_VERIFY(!c.response().value("busy").toBool());
                QCOMPARE(c.response().value("phase").toString(), QString("uncertain")); QCOMPARE(c.f.posts, before + 1);
            }
            if (mode.startsWith("new-default")) {
                c.f.stream["default_topic_uuid"] = topicId;
                auto event = cryptoEvent(c.f.stream); auto payload = event.value("payload").toObject();
                payload["kind"] = "stream.updated"; event["payload"] = payload; QVERIFY(c.f.controller.applyRealtimeEvent(event));
                QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(c.peer.requestDto)));
                QTRY_VERIFY(c.response().value("canRetry").toBool());
            }
            const auto streamGets = c.f.streamGets, bindingGets = c.f.bindingGets;
            c.f.controller.retryChatKeyResponse(c.peer.requestId);
            QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
            QCOMPARE(c.f.posts, before + 2); QCOMPARE(c.f.requestBody, frozen);
            QCOMPARE(c.f.streamGets, streamGets + 1); QCOMPARE(c.f.bindingGets, bindingGets + 1);
            QCOMPARE(exactRequests(c.f, "GET", "/api/workspace/v1/messenger/messages/" + c.peer.requestId), 1);
            QCOMPARE(c.f.requestBody.value("topic_uuid").toString(), secondTopicId);
        }
        const auto result = c.finishActual(); QVERIFY(result.value("ok").toBool()); QVERIFY(result.value("matchesExpected").toBool());
    }

    void issuerDeletedOriginalTopicCannotAuthorizeRetry() {
        IssuerCase c; QVERIFY(c.start()); QTRY_VERIFY(c.row().value("canGrant").toBool()); UncertainIssuerPost uncertain(c.f, false);
        c.f.controller.grantChatKey(c.peer.requestId); QTRY_COMPARE(c.response().value("phase").toString(), QString("uncertain"));
        const auto before = c.f.posts;
        auto event = cryptoEvent(QJsonObject{{"uuid", secondTopicId}, {"stream_uuid", streamId}});
        auto payload = event.value("payload").toObject(); payload["kind"] = "topic.deleted"; event["payload"] = payload;
        QVERIFY(c.f.controller.applyRealtimeEvent(event));
        QVERIFY(!c.response().value("canRetry").toBool()); c.f.controller.retryChatKeyResponse(c.peer.requestId);
        QTRY_VERIFY(!c.response().value("busy").toBool()); QCOMPARE(c.f.posts, before);
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void issuerTerminalStatusNeverProvidesRetryAuthority() {
        IssuerCase c; QVERIFY(c.start()); QTRY_VERIFY(c.row().value("canGrant").toBool()); UncertainIssuerPost uncertain(c.f, false);
        c.f.controller.grantChatKey(c.peer.requestId); QTRY_COMPARE(c.response().value("phase").toString(), QString("uncertain"));
        const auto path = "/api/workspace/v1/messenger/messages/" + uncertain.canonical.value("uuid").toString();
        const auto previous = c.f.http.handler; int status = 401;
        c.f.http.handler = [&](const Request &request) {
            auto reply = previous(request);
            if (request.method == "GET" && request.target.path() == path) {
                reply.status = status; reply.data = QJsonObject{{"uuid", OtherGeneration}};
            }
            return reply;
        };
        const auto restore = qScopeGuard([&] { c.f.http.handler = previous; });
        const auto before = c.f.posts; const auto gets = exactRequests(c.f, "GET", path);
        int count = 0;
        for (int value : {401, 403, 200}) {
            status = value; ++count; c.f.controller.retryChatKeyResponse(c.peer.requestId);
            QTRY_COMPARE(exactRequests(c.f, "GET", path), gets + count);
            QTRY_VERIFY(!c.response().value("busy").toBool());
            QCOMPARE(c.response().value("phase").toString(), QString("uncertain")); QCOMPARE(c.f.posts, before);
        }
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void issuerRealWssEchoBeforeAckSettlesWithoutAnotherGet() {
        IssuerCase c; QVERIFY(c.start()); RealtimeTransport transport(c.f); QVERIFY(transport.start());
        transport.publish(11, "message.created", c.peer.requestDto);
        QTRY_VERIFY(c.row().value("canGrant").toBool());
        c.f.postDelay = 1200; const auto before = c.f.posts; c.f.controller.grantChatKey(c.peer.requestId);
        QTRY_COMPARE(c.f.posts, before + 1); const auto row = c.f.savedRequest; const auto wire = c.f.requestBody;
        const auto path = "/api/workspace/v1/messenger/messages/" + row.value("uuid").toString();
        const auto gets = exactRequests(c.f, "GET", path);
        transport.publish(12, "message.created", row);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(12));
        QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.f.posts, before + 1); QCOMPARE(exactRequests(c.f, "GET", path), gets);
        const auto result = c.peer.finish(row, wire.value("uuid").toString());
        QVERIFY(result.value("ok").toBool()); QVERIFY(result.value("matchesExpected").toBool());
    }

    void issuerChangedRequestCannotPublishDelayedConfirmation_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"deleted", "moved-out", "device-ABA"}) QTest::newRow(mode) << QString(mode);
    }
    void issuerChangedRequestCannotPublishDelayedConfirmation() {
        QFETCH(QString, mode); IssuerCase c; QVERIFY(c.start()); QTRY_VERIFY(c.row().value("canGrant").toBool());
        c.f.checkDelay = 300; const auto before = c.f.posts; c.f.controller.grantChatKey(c.peer.requestId);
        QTRY_COMPARE(c.f.posts, before + 1);
        const auto path = "/api/workspace/v1/messenger/messages/" + c.f.savedRequest.value("uuid").toString();
        QTRY_COMPARE(exactRequests(c.f, "GET", path), 1);
        if (mode == "deleted") {
            auto event = cryptoEvent(QJsonObject{{"uuid", c.peer.requestId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}});
            auto payload = event.value("payload").toObject(); payload["kind"] = "message.deleted"; event["payload"] = payload;
            QVERIFY(c.f.controller.applyRealtimeEvent(event));
        } else {
            auto changed = c.peer.requestDto;
            if (mode == "moved-out") {
                auto stream = c.f.stream; stream["uuid"] = OtherGeneration; stream["default_topic_uuid"] = messageId;
                auto event = cryptoEvent(stream); auto payload = event.value("payload").toObject(); payload["kind"] = "stream.created";
                event["payload"] = payload; QVERIFY(c.f.controller.applyRealtimeEvent(event));
                changed["stream_uuid"] = OtherGeneration; changed["topic_uuid"] = messageId;
            } else {
                auto payload = changed.value("payload").toObject(); payload["device_id"] = "synthetic-changed-device"; changed["payload"] = payload;
            }
            QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(changed)));
            if (mode == "device-ABA") QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(c.peer.requestDto)));
        }
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.response().value("phase").toString(), QString("uncertain")); QCOMPARE(c.f.posts, before + 1);
        QVERIFY(!c.response().value("busy").toBool());
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void issuerLateLifetimeCannotPublish_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"owner", "api-generation", "cache-service", "vault-lease"}) QTest::newRow(mode) << QString(mode);
    }
    void issuerLateLifetimeCannotPublish() {
        QFETCH(QString, mode); IssuerCase c; QVERIFY(c.start()); QTRY_VERIFY(c.row().value("canGrant").toBool());
        NativeCacheService cache(c.f.directory.filePath("issuer-cache"));
        const auto cleanup = qScopeGuard([&] { c.f.controller.useCache(nullptr); cache.shutdown(); QTest::qWaitFor([&] { return cache.stopped(); }, 2000); });
        c.f.checkDelay = 300; const auto before = c.f.posts; c.f.controller.grantChatKey(c.peer.requestId);
        QTRY_COMPARE(c.f.posts, before + 1);
        QTRY_COMPARE(exactRequests(c.f, "GET", "/api/workspace/v1/messenger/messages/" + c.f.savedRequest.value("uuid").toString()), 1);
        if (mode == "owner") c.f.session.logout();
        else if (mode == "api-generation") {
            const auto generation = c.f.api.generation(); c.f.api.setAccessToken("synthetic-rotated-issuer-token"); QVERIFY(c.f.api.generation() > generation);
        } else if (mode == "cache-service") c.f.controller.useCache(&cache);
        else c.f.vault.close();
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(c.response().value("phase").toString() != "ready"); QCOMPARE(c.f.posts, before + 1);
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
        c.f.controller.useCache(nullptr); cache.shutdown(); QTRY_VERIFY(cache.stopped());
    }

    void issuerReadyPublicationIsReentrantSafe_data() {
        QTest::addColumn<bool>("navigate");
        QTest::newRow("new-request-intent") << false; QTest::newRow("navigate-away") << true;
    }
    void issuerReadyPublicationIsReentrantSafe() {
        QFETCH(bool, navigate); IssuerCase c; QVERIFY(c.start()); QTRY_VERIFY(c.row().value("canGrant").toBool());
        auto second = c.peer.requestDto;
        const auto local = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto secondId = QUuid::createUuidV5(QUuid(topicId), local.toUtf8()).toString(QUuid::WithoutBraces);
        second["uuid"] = secondId; c.f.savedRequests[secondId] = second;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(second)));
        QJsonObject firstResponse, firstBody; bool reentered = false;
        const auto previous = c.f.http.handler;
        c.f.http.handler = [&](const Request &request) {
            auto reply = previous(request);
            if (request.method == "POST" && request.body.value("payload").toObject().value("request_message_uuid").toString() == c.peer.requestId) {
                firstResponse = reply.data.toObject(); firstBody = request.body;
            }
            return reply;
        };
        const auto restore = qScopeGuard([&] { c.f.http.handler = previous; });
        const auto connection = connect(&c.f.controller, &NativeController::stateChanged, this, [&] {
            if (reentered || c.response().value("requestId").toString() != c.peer.requestId || c.response().value("phase").toString() != "ready") return;
            reentered = true;
            if (navigate) c.f.controller.selectStream({}); else c.f.controller.rejectChatKey(secondId);
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        const auto before = c.f.posts; c.f.controller.grantChatKey(c.peer.requestId); QTRY_VERIFY(reentered);
        if (navigate) { QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty()); QVERIFY(c.f.controller.selectedStream().isEmpty()); }
        else {
            QTRY_COMPARE(c.response().value("requestId").toString(), secondId);
            QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
            QCOMPARE(c.row().value("status").toString(), QString("granted"));
        }
        QCOMPARE(c.f.posts, before + (navigate ? 1 : 2));
        const auto result = c.peer.finish(firstResponse, firstBody.value("uuid").toString());
        QVERIFY(result.value("ok").toBool()); QVERIFY(result.value("matchesExpected").toBool());
    }

    void passiveMixedOutcomesFollowTheIndependentlyCheckedSourceRuntimeOrder_data() {
        QTest::addColumn<bool>("grantFirst");
        QTest::newRow("grant-then-reject") << true; QTest::newRow("reject-then-grant") << false;
    }
    void passiveMixedOutcomesFollowTheIndependentlyCheckedSourceRuntimeOrder() {
        QFETCH(bool, grantFirst); MixedOutcomeCase c; QVERIFY(c.start());
        const auto posts = c.f.posts, details = c.f.streamGets, bindings = c.f.bindingGets;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.peer.requestDto)));
        QTRY_VERIFY(!c.row().isEmpty());
        QStringList trace{c.row().value("status").toString()};
        QCOMPARE(trace.first(), QString("pending"));
        const auto first = grantFirst ? c.grant : c.reject, second = grantFirst ? c.reject : c.grant;
        for (const auto &response : {first, second}) {
            QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(response)));
            trace.append(c.row().value("status").toString());
        }
        // Collect every public step before comparing: no expected-status QTRY
        // can skip the contradictory second outcome or stop its admission.
        QCOMPARE(c.f.posts, posts); QCOMPARE(c.f.streamGets, details); QCOMPARE(c.f.bindingGets, bindings);
        QVERIFY(!c.row().value("canGrant").toBool()); QVERIFY(!c.row().value("canReject").toBool());
        const auto firstStatus = grantFirst ? QString("granted") : QString("rejected");
        QCOMPARE(trace, (QStringList{"pending", firstStatus, firstStatus}));
    }

    void updatingAnExistingOutcomeUuidRemainsCorrelatedWithoutAnotherSend() {
        MixedOutcomeCase c; QVERIFY(c.start());
        const auto posts = c.f.posts, details = c.f.streamGets, bindings = c.f.bindingGets;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.peer.requestDto)));
        QTRY_VERIFY(!c.row().isEmpty());
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.grant)));
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.reject)));
        auto updated = c.grant; updated["updated_at"] = "2026-01-03T00:00:00Z";
        QCOMPARE(updated.value("uuid").toString(), c.grant.value("uuid").toString());
        QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(updated)));
        QCOMPARE(c.row().value("status").toString(), QString("granted"));
        QCOMPARE(c.row().value("id").toString(), c.peer.requestId);
        QCOMPARE(c.row().value("keyUuid").toString(), returnedId);
        QVERIFY(!c.row().value("canGrant").toBool()); QVERIFY(!c.row().value("canReject").toBool());
        QCOMPARE(c.f.posts, posts); QCOMPARE(c.f.streamGets, details); QCOMPARE(c.f.bindingGets, bindings);
    }

    void invalidFirstOutcomeFallsBackAndSameUuidRestoreKeepsItsPosition_data() {
        QTest::addColumn<bool>("grantFirst");
        QTest::newRow("grant-invalid-restore") << true;
        QTest::newRow("reject-invalid-restore") << false;
    }
    void invalidFirstOutcomeFallsBackAndSameUuidRestoreKeepsItsPosition() {
        QFETCH(bool, grantFirst); MixedOutcomeCase c; QVERIFY(c.start());
        const auto posts = c.f.posts, details = c.f.streamGets, bindings = c.f.bindingGets;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.peer.requestDto)));
        QTRY_VERIFY(!c.row().isEmpty());
        QStringList trace{c.row().value("status").toString()};
        const auto first = grantFirst ? c.grant : c.reject, second = grantFirst ? c.reject : c.grant;
        for (const auto &response : {first, second}) {
            QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(response)));
            trace.append(c.row().value("status").toString());
        }
        auto invalid = first; auto payload = invalid.value("payload").toObject();
        payload["recipient_device_id"] = "different-recipient-device";
        invalid["payload"] = payload; invalid["updated_at"] = "2026-01-04T00:00:00Z";
        QVERIFY(parseMessage(invalid, projectId));
        QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(invalid)));
        trace.append(c.row().value("status").toString());
        auto restored = first; restored["updated_at"] = "2026-01-05T00:00:00Z";
        QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(restored)));
        trace.append(c.row().value("status").toString());
        const auto firstStatus = grantFirst ? QString("granted") : QString("rejected");
        const auto secondStatus = grantFirst ? QString("rejected") : QString("granted");
        QCOMPARE(trace, (QStringList{"pending", firstStatus, firstStatus, secondStatus, firstStatus}));
        QVERIFY(!c.row().value("canGrant").toBool()); QVERIFY(!c.row().value("canReject").toBool());
        QCOMPARE(c.f.posts, posts); QCOMPARE(c.f.streamGets, details); QCOMPARE(c.f.bindingGets, bindings);
    }

    void currentIssuerRoleRefreshRereducesTheOriginalPublicOrder() {
        MixedOutcomeCase c;
        // A binding demotion must not accidentally retain actual-owner rights.
        c.f.stream["owner"] = userId; c.f.issuerRole = "administrator";
        QVERIFY(c.start());
        const auto posts = c.f.posts, details = c.f.streamGets, bindings = c.f.bindingGets;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.peer.requestDto)));
        QTRY_VERIFY(!c.row().isEmpty());
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.grant)));
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.reject)));
        QStringList trace{c.row().value("status").toString()};
        for (const auto &role : {QString("member"), QString("administrator")}) {
            const auto beforeDetail = c.f.streamGets, beforeBindings = c.f.bindingGets;
            c.f.issuerRole = role;
            auto event = selfBindingEvent("stream_binding.updated");
            auto binding = event.value("payload").toObject();
            binding["uuid"] = returnedId; binding["user_uuid"] = Issuer;
            binding["who_uuid"] = userId; binding["role"] = role; event["payload"] = binding;
            QVERIFY(c.f.controller.applyRealtimeEvent(event));
            QTRY_COMPARE(c.f.streamGets, beforeDetail + 1);
            QTRY_COMPARE(c.f.bindingGets, beforeBindings + 1);
            QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
            QTRY_VERIFY(!c.row().isEmpty() && c.row().value("canManageKeys").toBool());
            trace.append(c.row().value("status").toString());
        }
        QCOMPARE(trace, (QStringList{"granted", "pending", "granted"}));
        QCOMPARE(c.f.posts, posts); QCOMPARE(c.f.streamGets, details + 2); QCOMPARE(c.f.bindingGets, bindings + 2);
    }

    void movedAndDeletedOutcomeCannotResurrectItsOldPublicCorrelation() {
        MixedOutcomeCase c; QVERIFY(c.start());
        const auto posts = c.f.posts, details = c.f.streamGets, bindings = c.f.bindingGets;
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.peer.requestDto)));
        QTRY_VERIFY(!c.row().isEmpty());
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.grant)));
        QVERIFY(c.f.controller.applyRealtimeEvent(cryptoEvent(c.reject)));
        QStringList trace{c.row().value("status").toString()};
        auto otherStream = c.f.stream; otherStream["uuid"] = OtherGeneration;
        otherStream["default_topic_uuid"] = messageId;
        auto streamEvent = cryptoEvent(otherStream); auto streamPayload = streamEvent.value("payload").toObject();
        streamPayload["kind"] = "stream.created"; streamEvent["payload"] = streamPayload;
        QVERIFY(c.f.controller.applyRealtimeEvent(streamEvent));
        auto moved = c.grant; moved["stream_uuid"] = OtherGeneration; moved["topic_uuid"] = messageId;
        moved["updated_at"] = "2026-01-04T00:00:00Z";
        QVERIFY(parseMessage(moved, projectId));
        QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(moved)));
        trace.append(c.row().value("status").toString());
        auto restored = c.grant; restored["updated_at"] = "2026-01-05T00:00:00Z";
        QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(restored)));
        trace.append(c.row().value("status").toString());
        auto deleted = cryptoEvent(QJsonObject{{"uuid", c.grant.value("uuid")},
            {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}});
        auto payload = deleted.value("payload").toObject(); payload["kind"] = "message.deleted"; deleted["payload"] = payload;
        QVERIFY(c.f.controller.applyRealtimeEvent(deleted));
        trace.append(c.row().value("status").toString());
        restored["updated_at"] = "2026-01-06T00:00:00Z";
        QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(restored)));
        trace.append(c.row().value("status").toString());
        QCOMPARE(trace, (QStringList{"granted", "rejected", "granted", "rejected", "rejected"}));
        QVERIFY(!c.row().value("canGrant").toBool()); QVERIFY(!c.row().value("canReject").toBool());
        QCOMPARE(c.f.posts, posts); QCOMPARE(c.f.streamGets, details); QCOMPARE(c.f.bindingGets, bindings);
    }

    void acceptedHistoryOrderSurvivesDisplaySortingAndInitialVaultReady_data() {
        QTest::addColumn<QString>("stage");
        QTest::newRow("ready-vault-before-history") << QString("ready");
        QTest::newRow("history-before-first-null-attachment") << QString("attach");
        QTest::newRow("history-before-attached-first-open") << QString("open");
    }
    void acceptedHistoryOrderSurvivesDisplaySortingAndInitialVaultReady() {
        QFETCH(QString, stage);
        MixedOutcomeCase source; QVERIFY(source.start()); // Actual Source public envelopes; no key is copied to f.
        const auto first = source.grant.value("uuid").toString() > source.reject.value("uuid").toString()
            ? source.grant : source.reject;
        const auto second = first.value("uuid") == source.grant.value("uuid") ? source.reject : source.grant;
        QVERIFY(first.value("uuid").toString() > second.value("uuid").toString());
        QCOMPARE(first.value("created_at").toString(), second.value("created_at").toString());
        const QJsonArray input{source.peer.requestDto, first, second};
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        addSourceRecipientBinding(f);
        const auto previous = f.http.handler;
        f.http.handler = [previous, input](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/") return Response{input};
            return previous(request);
        };
        const auto restore = qScopeGuard([&] { f.http.handler = previous; });
        if (stage == "open") f.controller.useChatKeys(&f.vault);
        f.controller.login(f.tls.origin().toString(), "Synthetic", "synthetic-password", "");
        QTRY_COMPARE(f.controller.projects().size(), 1); f.controller.openProject(projectId);
        QTRY_COMPARE(f.controller.streams().size(), 1);
        const auto open = [&] {
            const auto completed = std::make_shared<bool>(false);
            f.vault.open(f.tls.origin(), projectId, userId, f.deviceName,
                [completed](NativeChatKeyVault::Failure failure) { *completed = failure == NativeChatKeyVault::Failure::None; });
            return completed;
        };
        if (stage == "ready") {
            const auto opened = open(); QTRY_VERIFY(*opened); f.controller.useChatKeys(&f.vault);
        }
        f.controller.selectStream(streamId);
        QTRY_VERIFY(!f.controller.busy() && f.controller.messages().size() == 3);
        QCOMPARE(f.controller.selectedTopic(), QString{});
        QStringList displayResponses;
        for (const auto &row : f.controller.messages()) {
            const auto id = row.toMap().value("id").toString();
            if (id == first.value("uuid").toString() || id == second.value("uuid").toString()) displayResponses.append(id);
        }
        QCOMPARE(displayResponses, (QStringList{second.value("uuid").toString(), first.value("uuid").toString()}));
        if (stage != "ready") {
            QVERIFY(f.controller.encryption().value("keyRequests").toList().isEmpty());
            const auto opened = open(); QTRY_VERIFY(*opened);
            if (stage == "attach") f.controller.useChatKeys(&f.vault);
        }
        const auto row = [&] { return incomingKeyRequestRow(f.controller, source.peer.requestId); };
        QTRY_VERIFY(!row().isEmpty() && row().value("canManageKeys").toBool());
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        const auto expected = first.value("payload").toObject().value("kind").toString() == "key_grant"
            ? QString("granted") : QString("rejected");
        QCOMPARE(row().value("status").toString(), expected);
        QVERIFY(!row().value("canGrant").toBool()); QVERIFY(!row().value("canReject").toBool());
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/"), 1);
    }

    void repeatedAuthorizedOutcomesDoNotConsumeTheRequestWindowAgain() {
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f, true));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY(incomingKeyRequestRow(f.controller, peer.requestId).value("canReject").toBool());
        const auto beforePosts = f.posts;
        const auto beforeHttp = f.http.requests.size();
        const auto beforeRows = f.controller.messages().size();
        QVERIFY(beforeRows <= 2);
        QVector<QJsonObject> updates;
        for (int i = 0; i < 2; ++i) {
            const auto canonical = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto publicId = QUuid::createUuidV5(QUuid(secondTopicId), canonical.toUtf8()).toString(QUuid::WithoutBraces);
            auto response = message({}, publicId, secondTopicId);
            response["author_uuid"] = Issuer; response["is_own"] = false; response["user_uuid"] = userId;
            response["payload"] = QJsonObject{{"kind", "key_reject"}, {"key_uuid", returnedId},
                {"request_message_uuid", peer.requestId}, {"sender_device_id", IssuerDevice},
                {"recipient_device_id", SourceRecipientDevice}};
            QVERIFY(parseMessage(response, projectId));
            auto event = cryptoEvent(response); auto payload = event.value("payload").toObject();
            payload["kind"] = "message.updated"; event["payload"] = payload; updates.append(event);
        }
        // Real2m UTF16 budget, only2 response UUIDs and one externally requested
        // generation. Each old replacement erroneously leaks8+36=44units;
        // 46000x44=2024000 exceeds the real cap without increasing live rows.
        for (int i = 0; i < 46000; ++i) {
            auto event = updates.at(i % 2);
            event["uuid"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVERIFY(f.controller.applyRealtimeEvent(event));
        }
        QCOMPARE(f.controller.messages().size(), beforeRows);
        QCOMPARE(f.posts, beforePosts); QCOMPARE(f.http.requests.size(), beforeHttp);
        const auto row = incomingKeyRequestRow(f.controller, peer.requestId);
        QVERIFY2(!row.isEmpty(), "Replacing2valid response witnesses must not permanently exhaust the request budget");
        QCOMPARE(row.value("status").toString(), QString("rejected"));
        QVERIFY(!row.value("canGrant").toBool()); QVERIFY(!row.value("canReject").toBool());
        const auto result = peer.finish(); QVERIFY(result.value("ok").toBool());
        QVERIFY(result.value("recipientStillPending").toBool()); QVERIFY(!result.value("recipientHasKey").toBool());
        QCOMPARE(f.posts, beforePosts);
    }

    void independentSourceRecipientPeerKeepsExistingNativeGrantRead() {
        Fixture f; f.stream = fullEncryptedStream(); const auto roles = addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f, true));
        QCOMPARE(roles->value(userId), f.stream.value("role").toString());
        QCOMPARE(roles->value(Issuer), f.issuerRole);
        QCOMPARE(roles->value(SourceRecipient), QString("member"));
        QCOMPARE(displayed(f.controller, messageId).value("content").toString(), peer.expectedText);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.posts, 1); // Existing recipient request only, never an auto grant.
        const auto completed = peer.finish(); QVERIFY(completed.value("ok").toBool());
        QVERIFY(completed.value("recipientStillPending").toBool());
        QVERIFY(!completed.value("recipientHasKey").toBool()); QCOMPARE(completed.value("completionCount").toInt(), 0);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts, 1);
    }

// Additive slots proposal only; append inside NativeE2eeReadTest. NOT BUILT/RUN.
// Uses the unchanged actual TLS fixture and pinned SourceRecipientPeer mode.
// No requestChatKey(), put(), import(), initialize(), or native group-key seed.
// Additive slots only; old capability assertions are not changed.
    void ordinaryNonManagersWithoutACannotReject_data() {
        QTest::addColumn<QString>("role");
        QTest::newRow("member-no-A") << QString("member");
        QTest::newRow("moderator-no-A") << QString("moderator");
    }
    void ordinaryNonManagersWithoutACannotReject() {
        QFETCH(QString, role); OrdinaryRejectCase c; c.f.stream["role"] = role; QVERIFY(c.start());
        QCOMPARE(c.servedRoles->value(userId), role); QCOMPARE(c.servedRoles->value(SourceRecipient), QString("member"));
        QCOMPARE(c.row().value("status").toString(), QString("pending"));
        QVERIFY(!c.row().value("hasRequestedKey").toBool()); QVERIFY(!c.row().value("canManageKeys").toBool());
        QVERIFY(!c.row().value("offerGrant").toBool()); QVERIFY(!c.row().value("offerReject").toBool());
        c.f.controller.grantChatKey(c.peer.requestId); c.f.controller.rejectChatKey(c.peer.requestId);
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(c.f.posts, 0);
        const auto result = c.peer.finish(); QVERIFY(result.value("ok").toBool());
        QVERIFY(result.value("recipientStillPending").toBool()); QVERIFY(!result.value("recipientHasKey").toBool());
    }

    void ordinaryRejectNeedsFreshManagerAuthorityAfterDemotionAndRestore() {
        OrdinaryRejectCase c; QVERIFY(c.start()); QVERIFY(c.row().value("canReject").toBool());
        c.f.streamDelay = 1200;
        for (const auto &role : {QString("member"), QString("administrator")}) {
            const auto details = c.f.streamGets, bindings = c.f.bindingGets;
            c.f.stream["role"] = role; QVERIFY(c.streamUpdated());
            QTRY_COMPARE(c.f.streamGets, details + 1); // real TLS handler acknowledgment first
            QTRY_VERIFY(c.pending("GET", "/api/workspace/v1/messenger/streams/" + streamId));
            QVERIFY(!c.row().value("canReject").toBool());
            c.f.controller.rejectChatKey(c.peer.requestId); QCOMPARE(c.f.posts, 0);
            QTRY_COMPARE(c.f.bindingGets, bindings + 1);
            QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
            QTRY_VERIFY(!c.row().isEmpty()); QCOMPARE(c.servedRoles->value(userId), role);
            QCOMPARE(c.row().value("canReject").toBool(), role == "administrator");
            QVERIFY(!c.row().value("offerGrant").toBool()); QCOMPARE(c.f.posts, 0);
        }
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void ordinaryRequesterRemovalRevokesRejectBeforeDelayedAuthorityReturns() {
        OrdinaryRejectCase c; QVERIFY(c.start()); QVERIFY(c.row().value("canReject").toBool());
        const auto details = c.f.streamGets, bindings = c.f.bindingGets;
        *c.requesterMember = false; c.f.streamDelay = 1200;
        auto event = selfBindingEvent("stream_binding.deleted"); auto row = event.value("payload").toObject();
        row["uuid"] = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        row["user_uuid"] = SourceRecipient; row["who_uuid"] = userId; event["payload"] = row;
        QVERIFY(c.f.controller.applyRealtimeEvent(event));
        QTRY_COMPARE(c.f.streamGets, details + 1);
        QTRY_VERIFY(c.pending("GET", "/api/workspace/v1/messenger/streams/" + streamId));
        QVERIFY(!c.row().value("canReject").toBool()); c.f.controller.rejectChatKey(c.peer.requestId); QCOMPARE(c.f.posts, 0);
        QTRY_COMPARE(c.f.bindingGets, bindings + 1); QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!c.servedRoles->contains(SourceRecipient));
        QTRY_VERIFY(!c.row().isEmpty()); QVERIFY(!c.row().value("canManageKeys").toBool());
        QVERIFY(!c.row().value("canReject").toBool()); c.f.controller.rejectChatKey(c.peer.requestId); QCOMPARE(c.f.posts, 0);
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void ordinaryRejectCheckAndRetryKeepTheExactFrozenResponse_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"applied-503-GET-only", "absent-Check-then-identical-Retry", "new-default-retains-original-T"})
            QTest::newRow(mode) << QString(mode);
    }
    void ordinaryRejectCheckAndRetryKeepTheExactFrozenResponse() {
        QFETCH(QString, mode); OrdinaryRejectCase c; QVERIFY(c.start()); QVERIFY(c.row().value("canReject").toBool());
        const bool applied = mode.startsWith("applied"); UncertainIssuerPost uncertain(c.f, applied);
        c.f.controller.rejectChatKey(c.peer.requestId);
        QTRY_VERIFY(uncertain.failed); QTRY_COMPARE(c.response().value("phase").toString(), QString("uncertain"));
        const auto frozen = uncertain.frozen, canonical = uncertain.canonical;
        const auto path = "/api/workspace/v1/messenger/messages/" + canonical.value("uuid").toString();
        const auto gets = exactRequests(c.f, "GET", path); QCOMPARE(c.f.posts, 1);
        QVERIFY(c.response().value("canCheck").toBool()); c.f.controller.checkChatKeyResponse(c.peer.requestId);
        QTRY_COMPARE(exactRequests(c.f, "GET", path), gets + 1); QTRY_VERIFY(!c.response().value("busy").toBool());
        QCOMPARE(c.f.posts, 1);
        if (applied) QCOMPARE(c.response().value("phase").toString(), QString("ready"));
        else {
            QCOMPARE(c.response().value("phase").toString(), QString("uncertain"));
            if (mode.startsWith("new-default")) {
                c.f.stream["default_topic_uuid"] = topicId; QVERIFY(c.streamUpdated());
                QVERIFY(c.f.controller.applyRealtimeEvent(updatedControl(c.peer.requestDto)));
                QTRY_VERIFY(c.response().value("canRetry").toBool());
            }
            const auto details = c.f.streamGets, bindings = c.f.bindingGets;
            c.f.controller.retryChatKeyResponse(c.peer.requestId);
            QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
            QCOMPARE(c.f.posts, 2); QCOMPARE(c.f.requestBody, frozen);
            QCOMPARE(c.f.streamGets, details + 1); QCOMPARE(c.f.bindingGets, bindings + 1);
            QCOMPARE(exactRequests(c.f, "GET", "/api/workspace/v1/messenger/messages/" + c.peer.requestId), 1);
        }
        QCOMPARE(c.f.requestBody.value("topic_uuid").toString(), secondTopicId);
        QCOMPARE(c.f.requestBody.value("payload").toObject().value("kind").toString(), QString("key_reject"));
        const auto result = c.peer.finish(c.f.savedRequest, frozen.value("uuid").toString());
        QVERIFY(result.value("ok").toBool()); QCOMPARE(result.value("completionOutcome").toString(), QString("rejected"));
        QVERIFY(!result.value("recipientHasKey").toBool()); QCOMPARE(result.value("recipientPending").toInt(), 0);
    }

    void ordinaryRejectLatePostCannotPublishIntoAnotherLifetime_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("selected-topic") << QString("selected-topic");
        QTest::newRow("owner") << QString("owner");
    }
    void ordinaryRejectLatePostCannotPublishIntoAnotherLifetime() {
        QFETCH(QString, mode); OrdinaryRejectCase c; QVERIFY(c.start()); QVERIFY(c.row().value("canReject").toBool());
        c.f.postDelay = 1200; c.f.controller.rejectChatKey(c.peer.requestId);
        QTRY_COMPARE(c.f.posts, 1);
        QTRY_VERIFY(c.pending("POST", "/api/workspace/v1/messenger/messages/"));
        const auto publicId = c.f.savedRequest.value("uuid").toString(); QVERIFY(isUuid(publicId));
        if (mode == "owner") c.f.controller.logout(); else c.f.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(c.response().value("phase").toString() != "ready");
        QCOMPARE(c.f.posts, 1); QVERIFY(displayed(c.f.controller, publicId).isEmpty());
        QVERIFY(c.peer.finish().value("recipientStillPending").toBool());
    }

    void ordinaryManagerRejectsSourceRequestWithoutA_data() {
        QTest::addColumn<QString>("role");
        QTest::addColumn<bool>("actualOwner");
        QTest::newRow("actual-owner/member/no-A/current-null/default-different") << QString("member") << true;
        QTest::newRow("administrator/no-A/current-null/default-different") << QString("administrator") << false;
    }
    void ordinaryManagerRejectsSourceRequestWithoutA() {
        QFETCH(QString, role); QFETCH(bool, actualOwner);
        Fixture f; f.stream = fullEncryptedStream();
        f.stream["encryption"] = false;
        f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        f.stream["role"] = role;
        if (actualOwner) f.stream["owner"] = userId;
        f.cipher["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Synthetic ordinary history"}};
        const auto roles = addSourceRecipientBinding(f);
        useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f, false, OtherGeneration));
        // This is the actual Source-created public request, not a fabricated key pair.
        f.savedRequests.insert(peer.requestId, peer.requestDto);
        QCOMPARE(peer.requestDto.value("author_uuid").toString(), SourceRecipient);
        QCOMPARE(peer.requestDto.value("topic_uuid").toString(), topicId);
        QVERIFY(parseMessage(peer.requestDto, projectId));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).isEmpty());
        QTRY_VERIFY(incomingKeyRequestRow(f.controller, peer.requestId).value("canManageKeys").toBool());
        // Positive admission witness BEFORE the planned missing-action RED.
        QCOMPARE(roles->value(userId), role);
        QCOMPARE(roles->value(SourceRecipient), QString("member"));
        QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!f.stream.value("encryption").toBool());
        QVERIFY(f.stream.value("current_encryption_key_uuid").isNull());
        const auto row = incomingKeyRequestRow(f.controller, peer.requestId);
        QCOMPARE(row.value("status").toString(), QString("pending"));
        QCOMPARE(row.value("keyUuid").toString(), OtherGeneration);
        QVERIFY(!row.value("ownCurrentDevice").toBool());
        QVERIFY(!row.value("hasRequestedKey").toBool());
        QVERIFY(!row.value("offerGrant").toBool());
        auto missing = f.vault.read(f.vault.lease(), {streamId, OtherGeneration});
        const auto scrub = qScopeGuard([&] { missing.value.bytes.fill(0); });
        QCOMPARE(missing.failure, NativeChatKeyVault::Failure::None);
        QVERIFY(missing.value.bytes.isEmpty());
        QCOMPARE(f.posts, 0);
        QVERIFY2(row.value("offerReject").toBool(), "Planned ordinary manager Reject capability is unavailable");
        QVERIFY(row.value("canReject").toBool());
        const int details = f.streamGets, bindings = f.bindingGets, topics = f.topicGets;
        f.controller.rejectChatKey(peer.requestId);
        QTRY_COMPARE(f.posts, 1);
        QTRY_COMPARE(f.controller.encryption().value("keyResponse").toMap().value("phase").toString(), QString("ready"));
        QTRY_COMPARE(incomingKeyRequestRow(f.controller, peer.requestId).value("status").toString(), QString("rejected"));
        const auto body = f.requestBody, payload = body.value("payload").toObject();
        QCOMPARE(body.value("stream_uuid").toString(), streamId);
        QCOMPARE(body.value("topic_uuid").toString(), secondTopicId); // actual default, not R's topic
        QCOMPARE(payload, (QJsonObject{{"kind", "key_reject"}, {"key_uuid", OtherGeneration},
            {"request_message_uuid", peer.requestId}, {"sender_device_id", f.vault.device().id},
            {"recipient_device_id", SourceRecipientDevice}}));
        const auto canonical = body.value("uuid").toString(); QVERIFY(isUuid(canonical));
        const auto publicId = QUuid::createUuidV5(QUuid(secondTopicId), canonical.toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(f.savedRequest.value("uuid").toString(), publicId);
        QCOMPARE(f.savedRequest.value("user_uuid").toString(), userId);
        QCOMPARE(f.savedRequest.value("author_uuid").toString(), userId);
        QCOMPARE(f.savedRequest.value("payload").toObject(), payload);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 1);
        QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings); QCOMPARE(f.topicGets, topics);
        const auto result = peer.finish(f.savedRequest, canonical);
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("completionOutcome").toString(), QString("rejected"));
        QCOMPARE(result.value("completionRequest").toString(), peer.requestId);
        QCOMPARE(result.value("completionResponse").toString(), publicId);
        QCOMPARE(result.value("recipientPending").toInt(), 0);
        QVERIFY(!result.value("recipientHasKey").toBool());
        QVERIFY(!result.value("matchesExpected").toBool());
        // A completed R cannot create another response; duplicate click is not retry authority.
        f.controller.rejectChatKey(peer.requestId);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts, 1);
        QVERIFY(f.vault.read(f.vault.lease(), {streamId, OtherGeneration}).value.bytes.isEmpty());
    }

    void nativeIssuerGrantsExactGenerationToIndependentSourceRecipient_data() {
        QTest::addColumn<QString>("role"); QTest::addColumn<QString>("current"); QTest::addColumn<bool>("actualOwner");
        QTest::newRow("actual-owner-role-member/current-A/default-different") << QString("member") << QString("A") << true;
        QTest::newRow("administrator/historical-A-current-B/default-different") << QString("administrator") << QString("B") << false;
        QTest::newRow("administrator/historical-A-current-null/default-different") << QString("administrator") << QString("null") << false;
    }
    void nativeIssuerGrantsExactGenerationToIndependentSourceRecipient() {
        QFETCH(QString, role); QFETCH(QString, current); QFETCH(bool, actualOwner);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = role; f.issuerRole = "administrator";
        if (actualOwner) f.stream["owner"] = userId;
        const auto bindingRoles = addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f, true));
        QCOMPARE(bindingRoles->value(userId), f.stream.value("role").toString());
        QCOMPARE(bindingRoles->value(Issuer), f.issuerRole);
        QCOMPARE(bindingRoles->value(SourceRecipient), QString("member"));
        if (current != "A") {
            f.stream["current_encryption_key_uuid"] = current == "B" ? QJsonValue(OtherGeneration) : QJsonValue(QJsonValue::Null);
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
        }
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY2(!incomingKeyRequestRow(f.controller, peer.requestId).isEmpty(), "Missing planned public incoming key-request row");
        const auto row = incomingKeyRequestRow(f.controller, peer.requestId);
        QCOMPARE(row.value("authorId").toString(), SourceRecipient);
        QCOMPARE(row.value("deviceName").toString(), peer.requestDto.value("payload").toObject().value("device_name").toString());
        QCOMPARE(row.value("keyUuid").toString(), returnedId);
        for (const auto *field : {"publicKey", "privateKey", "content", "payload"}) QVERIFY(!row.contains(field));
        QTRY_VERIFY(incomingKeyRequestRow(f.controller, peer.requestId).value("canGrant").toBool());
        QVERIFY2(hasKeyResponseMethod(f.controller, "grantChatKey(QString)"), "Missing planned runtime grant method");
        const auto before = f.posts;
        QVERIFY(QMetaObject::invokeMethod(&f.controller, "grantChatKey", Qt::DirectConnection, Q_ARG(QString, peer.requestId)));
        QTRY_COMPARE(f.posts, before + 1);
        QTRY_COMPARE(incomingKeyRequestRow(f.controller, peer.requestId).value("status").toString(), QString("granted"));
        const auto body = f.requestBody, payload = body.value("payload").toObject();
        QCOMPARE(body.value("stream_uuid").toString(), streamId); QCOMPARE(body.value("topic_uuid").toString(), secondTopicId);
        QVERIFY(peer.requestDto.value("topic_uuid").toString() != body.value("topic_uuid").toString());
        QCOMPARE(payload.value("kind").toString(), QString("key_grant")); QCOMPARE(payload.value("key_uuid").toString(), returnedId);
        QCOMPARE(payload.value("request_message_uuid").toString(), peer.requestId);
        QCOMPARE(payload.value("sender_device_id").toString(), f.vault.device().id);
        QCOMPARE(payload.value("recipient_device_id").toString(), SourceRecipientDevice); QVERIFY(payload.value("content").isString());
        const auto result = peer.finish(f.savedRequest, body.value("uuid").toString()); QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("completionOutcome").toString(), QString("granted"));
        QCOMPARE(result.value("completionRequest").toString(), peer.requestId); QCOMPARE(result.value("recipientPending").toInt(), 0);
        QVERIFY(result.value("recipientHasKey").toBool()); QVERIFY(result.value("groupMatchesIssuer").toBool());
        QVERIFY(result.value("matchesExpected").toBool()); QCOMPARE(f.posts, before + 1);
    }

    void nativeIssuerRejectsUnknownHistoricalGenerationWithoutItsSecret() {
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        const auto bindingRoles = addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f, false, OtherGeneration));
        auto missing = f.vault.read(f.vault.lease(), {streamId, OtherGeneration});
        QCOMPARE(missing.failure, NativeChatKeyVault::Failure::None); QVERIFY(missing.value.bytes.isEmpty());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY2(!incomingKeyRequestRow(f.controller, peer.requestId).isEmpty(), "Missing planned public incoming key-request row");
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("canGrant").toBool());
        QTRY_VERIFY(incomingKeyRequestRow(f.controller, peer.requestId).value("canReject").toBool());
        QVERIFY2(hasKeyResponseMethod(f.controller, "rejectChatKey(QString)"), "Missing planned runtime reject method");
        const auto before = f.posts;
        QVERIFY(QMetaObject::invokeMethod(&f.controller, "rejectChatKey", Qt::DirectConnection, Q_ARG(QString, peer.requestId)));
        QTRY_COMPARE(f.posts, before + 1);
        QTRY_COMPARE(incomingKeyRequestRow(f.controller, peer.requestId).value("status").toString(), QString("rejected"));
        QCOMPARE(bindingRoles->value(userId), f.stream.value("role").toString());
        QCOMPARE(bindingRoles->value(Issuer), f.issuerRole);
        QCOMPARE(bindingRoles->value(SourceRecipient), QString("member"));
        const auto body = f.requestBody, payload = body.value("payload").toObject();
        QCOMPARE(payload.value("kind").toString(), QString("key_reject")); QCOMPARE(payload.value("key_uuid").toString(), OtherGeneration);
        QCOMPARE(payload.value("request_message_uuid").toString(), peer.requestId); QVERIFY(!payload.contains("content"));
        QCOMPARE(body.value("topic_uuid").toString(), secondTopicId);
        const auto result = peer.finish(f.savedRequest, body.value("uuid").toString()); QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("completionOutcome").toString(), QString("rejected"));
        QCOMPARE(result.value("completionRequest").toString(), peer.requestId); QCOMPARE(result.value("recipientPending").toInt(), 0);
        QVERIFY(!result.value("recipientHasKey").toBool()); QVERIFY(!result.value("matchesExpected").toBool());
        QCOMPARE(f.posts, before + 1);
    }

// Separate scope transition: encrypted moderator data + old function body stay
// byte-exact. Remove ONLY the ordinary data row from the old two-row _data().
// Add this named ordinary successor; the encryption flag RT transition here is
// explicitly a legacy synthetic setup, NOT a supported backend mutation/userflow.
    void ordinaryGrantExclusionSurvivesRenewedAuthorityWhileRejectBecomesAvailable() {
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        const auto roles = addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f, true)); // existing actual encrypted grant seed only
        auto held = f.vault.read(f.vault.lease(), {streamId, returnedId});
        const auto scrub = qScopeGuard([&] { held.value.bytes.fill(0); });
        QCOMPARE(held.failure, NativeChatKeyVault::Failure::None); QCOMPARE(held.value.bytes.size(), 32);
        const auto posts = f.posts, details = f.streamGets, bindings = f.bindingGets;
        f.streamDelay = 1200; f.stream["encryption"] = false; f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
        payload["kind"] = "stream.updated"; event["payload"] = payload; QVERIFY(f.controller.applyRealtimeEvent(event));
        f.savedRequests.insert(peer.requestId, peer.requestDto);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_COMPARE(f.streamGets, details + 1);
        const auto pendingDetail = [&] {
            for (auto *reply : f.api.findChildren<QNetworkReply *>())
                if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/streams/" + streamId
                    && reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "GET") return true;
            return false;
        };
        QTRY_VERIFY(pendingDetail());
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("canGrant").toBool());
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("canReject").toBool());
        f.controller.grantChatKey(peer.requestId); f.controller.rejectChatKey(peer.requestId); QCOMPARE(f.posts, posts);
        QTRY_COMPARE(f.bindingGets, bindings + 1); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_VERIFY(incomingKeyRequestRow(f.controller, peer.requestId).value("canReject").toBool());
        QCOMPARE(roles->value(userId), QString("administrator")); QCOMPARE(roles->value(SourceRecipient), QString("member"));
        QVERIFY(incomingKeyRequestRow(f.controller, peer.requestId).value("hasRequestedKey").toBool());
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("offerGrant").toBool());
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("canGrant").toBool());
        f.controller.grantChatKey(peer.requestId); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.posts, posts); // no metadata-driven auto reply and no ordinary Grant even with A
        QVERIFY(peer.finish().value("recipientStillPending").toBool());
    }

    void excludedIssuerPathsDoNotReplyMerelyBecauseTheyPossessA_data() {
        QTest::addColumn<bool>("ordinary");
        QTest::newRow("encrypted-moderator-with-A") << false;
    }
    void excludedIssuerPathsDoNotReplyMerelyBecauseTheyPossessA() {
        QFETCH(bool, ordinary);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = ordinary ? "administrator" : "moderator";
        const auto bindingRoles = addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f, true));
        QCOMPARE(bindingRoles->value(userId), f.stream.value("role").toString());
        QCOMPARE(bindingRoles->value(Issuer), f.issuerRole);
        QCOMPARE(bindingRoles->value(SourceRecipient), QString("member"));
        auto held = f.vault.read(f.vault.lease(), {streamId, returnedId});
        const auto scrub = qScopeGuard([&] { held.value.bytes.fill(0); });
        QCOMPARE(held.failure, NativeChatKeyVault::Failure::None); QCOMPARE(held.value.bytes.size(), 32);
        if (ordinary) {
            f.stream["encryption"] = false; f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
        }
        const auto before = f.posts; QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("canGrant").toBool());
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("canReject").toBool());
        // These are policy/scope controls, not compile-only missing capability REDs.
        // Old product has neither method; once present, direct calls must still be safe.
        if (hasKeyResponseMethod(f.controller, "grantChatKey(QString)"))
            QVERIFY(QMetaObject::invokeMethod(&f.controller, "grantChatKey", Qt::DirectConnection, Q_ARG(QString, peer.requestId)));
        if (hasKeyResponseMethod(f.controller, "rejectChatKey(QString)"))
            QVERIFY(QMetaObject::invokeMethod(&f.controller, "rejectChatKey", Qt::DirectConnection, Q_ARG(QString, peer.requestId)));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts, before);
        const auto result = peer.finish(); QVERIFY(result.value("ok").toBool());
        QVERIFY(result.value("recipientStillPending").toBool()); QVERIFY(!result.value("recipientHasKey").toBool());
        QCOMPARE(result.value("completionCount").toInt(), 0);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts, before);
    }

    void encryptedComposerDoesNotNeedADraftAndHistoricalSelectionFollowsCurrentAvailability() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString first = "Synthetic historical A while current B is missing";
        SourceSendPeer peerA; QVERIFY(peerA.prepare(f, topicId, first));
        QVERIFY(f.controller.draft().isEmpty()); QVERIFY(f.controller.canCompose()); QVERIFY(!f.controller.canSend());
        f.stream["current_encryption_key_uuid"] = OtherGeneration;
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
        payload["kind"] = "stream.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event));
        QTRY_VERIFY(f.controller.canCompose());
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
        const auto bindings = f.bindingGets;
        for (int i = 0; i < 100; ++i) { QVERIFY(f.controller.canCompose()); QVERIFY(!f.controller.canSend()); f.controller.encryption(); }
        QCOMPARE(f.bindingGets, bindings);
        f.controller.setDraft(first); QTRY_VERIFY(f.controller.canSend());
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        f.controller.send(); QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(f.requestBody.value("payload").toObject().value("key_uuid").toString(), returnedId);
        QVERIFY(peerA.verify(f.requestBody.value("payload").toObject(), topicId).value("matchesExpected").toBool());
        const QString second = "Synthetic current B after its real Source grant";
        SourceSendPeer peerB; QVERIFY(peerB.prepare(f, topicId, second, OtherGeneration));
        QTRY_VERIFY(f.controller.canCompose());
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), OtherGeneration);
        f.controller.setDraft(second); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_COMPARE(sent.size(), 2);
        QCOMPARE(f.requestBody.value("payload").toObject().value("key_uuid").toString(), OtherGeneration);
        QVERIFY(peerB.verify(f.requestBody.value("payload").toObject(), topicId).value("matchesExpected").toBool());
    }

    void encryptedTextRightsFollowOwnedSourceSemantics_data() {
        QTest::addColumn<QString>("change"); QTest::addColumn<bool>("allowed");
        for (const auto *role : {"guest", "member", "moderator", "administrator", "owner"})
            QTest::newRow(role) << QString(role) << true;
        QTest::newRow("archived") << QString("archived") << true;
        QTest::newRow("closed-explicit") << QString("closed") << false;
        QTest::newRow("null-current") << QString("null") << false;
        QTest::newRow("zulip-provider") << QString("zulip") << false;
        QTest::newRow("unknown-role") << QString("unknown") << false;
    }
    void encryptedTextRightsFollowOwnedSourceSemantics() {
        QFETCH(QString, change); QFETCH(bool, allowed);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString text = "Synthetic participant encrypted text";
        SourceSendPeer peer; QVERIFY(peer.prepare(f, topicId, text));
        if (change == "closed") {
            auto row = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"user_uuid", userId},
                {"stream_uuid", streamId}, {"name", "Visible history"}, {"is_default", false}, {"is_done", true}};
            auto event = cryptoEvent(row); auto payload = event.value("payload").toObject();
            payload["kind"] = "topic.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
        } else {
            if (change == "archived") f.stream["is_archived"] = true;
            else if (change == "null") f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
            else if (change == "zulip") { f.stream["source_name"] = "zulip"; f.stream["provider_identity"] = "synthetic-provider"; }
            else f.stream["role"] = change;
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
        }
        const auto posts = f.posts;
        f.controller.setDraft(text);
        if (!allowed) {
            QVERIFY(!f.controller.canCompose()); QVERIFY(!f.controller.canSend());
            f.controller.send(); QCOMPARE(f.posts, posts); QCOMPARE(f.controller.draft(), text);
            return;
        }
        QTRY_VERIFY(f.controller.canCompose()); QTRY_VERIFY(f.controller.canSend());
        QSignalSpy sent(&f.controller, &NativeController::messageSent); f.controller.send();
        QTRY_COMPARE(sent.size(), 1); QCOMPARE(f.posts, posts + 1);
        QVERIFY(peer.verify(f.requestBody.value("payload").toObject(), topicId).value("matchesExpected").toBool());
        for (const auto &request : f.http.requests) QVERIFY(!request.target.path().contains("unarchive"));
    }

    void encryptedCanonicalEchoMustMatchEveryFrozenIdentityField_data() {
        QTest::addColumn<QString>("field"); QTest::addColumn<bool>("onGet");
        for (const auto *field : {"uuid", "project", "viewer", "author", "stream", "topic", "kind", "key", "cipher"})
            for (bool get : {false, true}) QTest::newRow(qPrintable(QString(field) + (get ? "-GET" : "-POST"))) << QString(field) << get;
    }
    void encryptedCanonicalEchoMustMatchEveryFrozenIdentityField() {
        QFETCH(QString, field); QFETCH(bool, onGet);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start()); QVERIFY(f.prepareA());
        const auto previous = f.http.handler;
        f.http.handler = [&](const Request &request) {
            auto response = previous(request); const auto path = request.target.path();
            auto row = response.data.toObject();
            if (row.value("payload").toObject().value("kind") == "e2ee"
                && (onGet ? request.method == "GET" && path != "/api/workspace/v1/messenger/messages/" : request.method == "POST")) {
                if (field == "uuid") row["uuid"] = OtherGeneration;
                else if (field == "project") row["project_id"] = OtherGeneration;
                else if (field == "viewer") row["user_uuid"] = OtherGeneration;
                else if (field == "author") row["author_uuid"] = Issuer;
                else if (field == "stream") row["stream_uuid"] = OtherGeneration;
                else if (field == "topic") row["topic_uuid"] = secondTopicId;
                else { auto payload = row.value("payload").toObject();
                    if (field == "kind") payload = {{"kind", "markdown"}, {"content", "Synthetic substituted echo"}};
                    else if (field == "key") payload["key_uuid"] = OtherGeneration;
                    else payload["content"] = QString::fromLatin1(QByteArray(48, 'x').toBase64());
                    row["payload"] = payload;
                }
                response.data = row;
            }
            return response;
        };
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        const QString text = "Synthetic exact encrypted echo";
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_VERIFY(!f.controller.encryption().value("pendingSend").toMap().value("busy").toBool());
        QCOMPARE(f.controller.encryption().value("pendingSend").toMap().value("phase").toString(), QString("uncertain"));
        QCOMPARE(f.posts, 2); QCOMPARE(sent.size(), 0); QCOMPARE(f.controller.draft(), text);
        const auto id = f.savedRequest.value("uuid").toString();
        QVERIFY(displayed(f.controller, id).isEmpty()); QVERIFY(!f.controller.canSend());
    }

    void encryptedUncertaintyCheckNeverPostsAndExplicitRetryKeepsExactWire_data() {
        QTest::addColumn<bool>("committed");
        QTest::newRow("committed-503-GET-only") << true;
        QTest::newRow("absent-503-fresh-404-identical-retry") << false;
    }
    void encryptedUncertaintyCheckNeverPostsAndExplicitRetryKeepsExactWire() {
        QFETCH(bool, committed);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString text = "Synthetic frozen uncertainty and retry";
        SourceSendPeer peer; QVERIFY(peer.prepare(f, topicId, text));
        const auto previous = f.http.handler; bool failed = false;
        f.http.handler = [&](const Request &request) {
            auto response = previous(request);
            if (!failed && request.method == "POST" && request.body.value("payload").toObject().value("kind") == "e2ee") {
                failed = true; response.status = 503;
                if (!committed) f.savedRequests.remove(response.data.toObject().value("uuid").toString());
            }
            return response;
        };
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_VERIFY(failed && !f.controller.encryption().value("pendingSend").toMap().value("busy").toBool());
        const auto frozen = f.requestBody;
        f.controller.checkPendingSend();
        QTRY_VERIFY(!f.controller.encryption().value("pendingSend").toMap().value("busy").toBool());
        QCOMPARE(f.posts, 2);
        if (!committed) {
            QCOMPARE(sent.size(), 0); QCOMPARE(f.controller.draft(), text);
            QVERIFY(f.controller.encryption().value("pendingSend").toMap().value("canRetry").toBool());
            f.controller.retryPendingSend();
        }
        QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(f.posts, committed ? 2 : 3);
        QVERIFY(f.requestBody == frozen);
        QVERIFY(peer.verify(frozen.value("payload").toObject(), topicId).value("matchesExpected").toBool());
        QCOMPARE(f.controller.draft(), QString{});
    }

    void aPendingEncryptedIntentCannotBecomeAnOrdinaryRetry_data() {
        QTest::addColumn<bool>("encryptedIntent"); QTest::addColumn<bool>("editedDraft");
        QTest::newRow("encrypted-frozen-draft") << true << false;
        QTest::newRow("encrypted-edited-draft") << true << true;
        QTest::newRow("ordinary-same-intent-retry-control") << false << false;
        QTest::newRow("ordinary-edited-new-intent-control") << false << true;
    }
    void aPendingEncryptedIntentCannotBecomeAnOrdinaryRetry() {
        QFETCH(bool, encryptedIntent); QFETCH(bool, editedDraft);
        const QString text = "Synthetic unresolved typed message";
        const QString nextText = editedDraft ? "Synthetic edited unresolved message" : text;
        bool failed = false;
        Fixture f; f.stream = fullEncryptedStream();
        if (!encryptedIntent) {
            f.stream["encryption"] = false; f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
            f.cipher = message("Existing ordinary history");
        }
        useCompleteSendEcho(f); QVERIFY(f.start());
        if (encryptedIntent) QVERIFY(f.prepareA());
        const auto previous = f.http.handler;
        f.http.handler = [&, previous](const Request &request) {
            auto response = previous(request);
            if (!failed && request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/messages/"
                && request.body.value("payload").toObject().value("kind") == (encryptedIntent ? "e2ee" : "markdown")) {
                failed = true; response.status = 503;
                f.savedRequests.remove(response.data.toObject().value("uuid").toString());
            }
            return response;
        };
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_VERIFY(failed && f.api.findChildren<QNetworkReply *>().isEmpty());
        const auto frozen = f.requestBody;
        const auto publicId = QUuid::createUuidV5(QUuid(topicId), frozen.value("uuid").toString().toUtf8())
            .toString(QUuid::WithoutBraces);
        QCOMPARE(sent.size(), 0); QCOMPARE(f.controller.draft(), text);
        if (encryptedIntent) {
            QCOMPARE(f.controller.encryption().value("pendingSend").toMap().value("phase").toString(), QString("uncertain"));
            f.stream["encryption"] = false; f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
            const auto parsed = parseStream(f.stream, projectId); QVERIFY(parsed); QVERIFY(!parsed->encrypted);
            QVERIFY(parsed->security); QVERIFY(parsed->security->currentKey.isEmpty());
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
            QVERIFY(!f.controller.streams().first().toMap().value("encrypted").toBool());
            QCOMPARE(f.controller.encryption().value("pendingSend").toMap().value("id").toString(), publicId);
        }
        f.controller.setDraft(nextText); QCOMPARE(f.controller.draft(), nextText);
        const auto before = f.posts; const bool allowed = f.controller.canSend();
        f.controller.send(); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        if (encryptedIntent) {
            qInfo("typed-intent transition: canSend=%d posts=%d->%d frozenUuidReused=%d lastKind=%s",
                allowed, before, f.posts, f.requestBody.value("uuid") == frozen.value("uuid"),
                qPrintable(f.requestBody.value("payload").toObject().value("kind").toString()));
            QCOMPARE(f.posts, before); QVERIFY(!allowed);
            QCOMPARE(sent.size(), 0); QCOMPARE(f.controller.draft(), nextText);
            f.stream["encryption"] = true; f.stream["current_encryption_key_uuid"] = returnedId;
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
            QTRY_COMPARE(f.controller.encryption().value("pendingSend").toMap().value("id").toString(), publicId);
            QVERIFY(!f.controller.canSend());
            return;
        }
        QVERIFY(allowed); QCOMPARE(f.posts, before + 1); QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(f.requestBody.value("payload").toObject(), QJsonObject({{"kind", "markdown"}, {"content", nextText}}));
        QCOMPARE(f.requestBody.value("uuid").toString() == frozen.value("uuid").toString(), !editedDraft);
        QCOMPARE(f.controller.draft(), QString{});
    }

    void exactRealtimeReceiptWinsBeforeDelayedHttpAckAndCannotBePublishedTwice() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString text = "Synthetic realtime-first encrypted receipt";
        SourceSendPeer peer; QVERIFY(peer.prepare(f, topicId, text));
        const auto previous = f.http.handler;
        f.http.handler = [&](const Request &request) {
            auto response = previous(request);
            if (request.method == "POST" && request.body.value("payload").toObject().value("kind") == "e2ee") {
                const auto row = response.data.toObject(); response.delay = 250; response.status = 503;
                QTimer::singleShot(0, &f.controller, [&, row] {
                    f.controller.applyRealtimeEvent(cryptoEvent(row));
                    f.controller.applyRealtimeEvent(cryptoEvent(row));
                });
            }
            return response;
        };
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_COMPARE(sent.size(), 1);
        const auto publicId = f.savedRequest.value("uuid").toString();
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 0);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(sent.size(), 1); QCOMPARE(f.posts, 2); QCOMPARE(f.controller.draft(), QString{});
        QVERIFY(f.controller.error().isEmpty());
        QVERIFY(peer.verify(f.requestBody.value("payload").toObject(), topicId).value("matchesExpected").toBool());
    }

    void encryptedMaterialDeletionAndReimportCannotReviveItsFrozenPendingSend() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start()); QVERIFY(f.prepareA());
        auto material = f.vault.read(f.vault.lease(), {streamId, returnedId});
        const auto wipe = qScopeGuard([&] { material.value.bytes.fill(0); });
        QCOMPARE(material.failure, NativeChatKeyVault::Failure::None);
        f.postDelay = 200;
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        f.controller.setDraft("Synthetic deletion ABA encrypted send"); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_COMPARE(f.posts, 2);
        QCOMPARE(f.vault.remove(f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
        auto deleted = f.vault.read(f.vault.lease(), {streamId, returnedId});
        QVERIFY(deleted.value.bytes.isEmpty());
        QCOMPARE(f.vault.put(f.vault.lease(), {streamId, returnedId}, material.value.bytes, deleted.value.deletionRevision),
            NativeChatKeyVault::Failure::None);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(sent.size(), 0); QVERIFY(!f.controller.draft().isEmpty());
        const auto pending = f.controller.encryption().value("pendingSend").toMap();
        QCOMPARE(pending.value("phase").toString(), QString("uncertain"));
        QVERIFY(!pending.value("canRetry").toBool()); f.controller.retryPendingSend(); QCOMPARE(f.posts, 2);
    }

    void encryptedSendAdmissionReentrantNavigationKeepsDraftAndDoesNotPost() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start()); QVERIFY(f.prepareA());
        bool moved = false;
        const auto connection = connect(&f.controller, &NativeController::stateChanged, this, [&] {
            if (!moved && f.controller.encryption().value("pendingSend").toMap().value("busy").toBool()) {
                moved = true; f.controller.selectTopic(secondTopicId);
            }
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        const QString text = "Synthetic retained encrypted draft after route interruption";
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QVERIFY(moved); QCOMPARE(f.posts, 1); QCOMPARE(f.controller.selectedTopic(), secondTopicId);
        f.controller.selectTopic(topicId);
        QTRY_VERIFY(!f.controller.busy()); QCOMPARE(f.controller.draft(), text);
        QVERIFY(!f.controller.encryption().value("pendingSend").toMap().value("busy").toBool());
    }

    void encryptedAggregateUsesAUniqueOwnedDefaultWhenTheDeclaredFieldIsNull() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString text = "Synthetic nullable-declared actual default";
        SourceSendPeer peer; QVERIFY(peer.prepare(f, secondTopicId, text));
        f.stream["default_topic_uuid"] = QJsonValue::Null;
        const auto previous = f.http.handler;
        f.http.handler = [previous](const Request &request) {
            auto response = previous(request);
            if (request.target.path() == "/api/workspace/v1/messenger/stream_topics/") {
                auto rows = response.data.toArray();
                for (int i = 0; i < rows.size(); ++i) { auto row = rows[i].toObject(); row["is_default"] = row.value("uuid").toString() == secondTopicId; rows[i] = row; }
                response.data = rows;
            }
            return response;
        };
        auto event = cryptoEvent(f.stream); auto body = event.value("payload").toObject(); body["kind"] = "stream.updated"; event["payload"] = body;
        QVERIFY(f.controller.applyRealtimeEvent(event)); f.controller.selectStream(streamId);
        QTRY_VERIFY(!f.controller.busy() && f.controller.topics().size() == 2);
        QVERIFY(f.controller.selectedTopic().isEmpty()); f.controller.setDraft(text);
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.canCompose(), 2000);
        QSignalSpy sent(&f.controller, &NativeController::messageSent); f.controller.send(); QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(f.requestBody.value("topic_uuid").toString(), secondTopicId);
        QVERIFY(peer.verify(f.requestBody.value("payload").toObject(), secondTopicId).value("matchesExpected").toBool());
    }

    void encryptedReadOnlyStatusDoesNotNeedTheDeletedKeyOrAnOpenVault_data() {
        QTest::addColumn<bool>("closeVault");
        QTest::newRow("deleted-key") << false; QTest::newRow("closed-vault") << true;
    }
    void encryptedReadOnlyStatusDoesNotNeedTheDeletedKeyOrAnOpenVault() {
        QFETCH(bool, closeVault);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start()); QVERIFY(f.prepareA());
        const auto previous = f.http.handler;
        f.http.handler = [previous](const Request &request) {
            auto response = previous(request);
            if (request.method == "POST" && request.body.value("payload").toObject().value("kind") == "e2ee") response.status = 503;
            return response;
        };
        const QString text = "Synthetic public delivery status without local key material";
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_VERIFY(f.posts == 2 && !f.controller.encryption().value("pendingSend").toMap().value("busy").toBool());
        if (closeVault) f.vault.close();
        else QCOMPARE(f.vault.remove(f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.encryption().value("pendingSend").toMap().value("canCheck").toBool(), 2000);
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        f.controller.checkPendingSend(); QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(f.posts, 2); QCOMPARE(f.controller.draft(), QString{});
        QVERIFY(f.controller.encryption().value("pendingSend").toMap().isEmpty()); QVERIFY(!f.controller.canCompose());
        const auto saved = displayed(f.controller, f.savedRequest.value("uuid").toString());
        QVERIFY(!saved.value("decrypted").toBool()); QVERIFY(saved.value("content").toString() != text);
    }

    void encryptedAggregateRetryKeepsTheOriginalOwnedTargetAfterDefaultChanges() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString text = "Synthetic frozen original aggregate target";
        SourceSendPeer peer; QVERIFY(peer.prepare(f, secondTopicId, text));
        f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy() && f.controller.topics().size() == 2);
        const auto previous = f.http.handler; bool failed = false;
        f.http.handler = [&](const Request &request) {
            auto response = previous(request);
            if (!failed && request.method == "POST" && request.body.value("payload").toObject().value("kind") == "e2ee") {
                failed = true; response.status = 503; f.savedRequests.remove(response.data.toObject().value("uuid").toString());
            }
            return response;
        };
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_VERIFY(failed && !f.controller.encryption().value("pendingSend").toMap().value("busy").toBool());
        const auto frozen = f.requestBody;
        f.stream["default_topic_uuid"] = topicId;
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject(); payload["kind"] = "stream.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event));
        for (const auto &topic : {topicId, secondTopicId}) {
            auto row = QJsonObject{{"uuid", topic}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Owned topic"}, {"is_done", false}, {"is_default", topic == topicId}};
            auto change = cryptoEvent(row); auto body = change.value("payload").toObject(); body["kind"] = "topic.updated"; change["payload"] = body;
            QVERIFY(f.controller.applyRealtimeEvent(change));
        }
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.encryption().value("pendingSend").toMap().value("canRetry").toBool(), 2000);
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        f.controller.retryPendingSend(); QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(f.posts, 3); QVERIFY(f.requestBody == frozen);
        QCOMPARE(f.requestBody.value("topic_uuid").toString(), secondTopicId);
        QVERIFY(peer.verify(f.requestBody.value("payload").toObject(), secondTopicId).value("matchesExpected").toBool());
    }

    void historicalSendingSelectionSurvivesANewerOldGrantBeforeAnySend() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString text = "Synthetic stable historical selection before dispatch";
        SourceSendPeer peerA; QVERIFY(peerA.prepare(f, topicId, text));
        f.stream["current_encryption_key_uuid"] = OtherGeneration;
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject(); payload["kind"] = "stream.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event)); QTRY_VERIFY(f.controller.canCompose());
        const QString oldC = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        SourceSendPeer peerC; QVERIFY(peerC.prepare(f, topicId, "Synthetic later old generation", oldC));
        QTRY_VERIFY(f.controller.canCompose());
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend());
        QSignalSpy sent(&f.controller, &NativeController::messageSent); f.controller.send(); QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(f.requestBody.value("payload").toObject().value("key_uuid").toString(), returnedId);
        QVERIFY(peerA.verify(f.requestBody.value("payload").toObject(), topicId).value("matchesExpected").toBool());
    }

    void encryptedReceiptSettlesItsOriginalConversationWhileAnotherRouteIsVisible_data() {
        QTest::addColumn<bool>("realtime");
        QTest::newRow("HTTP") << false; QTest::newRow("RT-before-ACK") << true;
    }
    void encryptedReceiptSettlesItsOriginalConversationWhileAnotherRouteIsVisible() {
        QFETCH(bool, realtime);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start()); QVERIFY(f.prepareA());
        f.postDelay = 150;
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        const QString text = "Synthetic page-independent original conversation receipt";
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend()); f.controller.send();
        QTRY_COMPARE(f.posts, 2); const auto canonical = f.savedRequest;
        f.controller.selectTopic(secondTopicId);
        const QString other = "New visible conversation draft"; f.controller.setDraft(other);
        if (realtime) QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(canonical)));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.selectedTopic(), secondTopicId); QCOMPARE(f.controller.draft(), other);
        QVERIFY(displayed(f.controller, canonical.value("uuid").toString()).isEmpty()); QCOMPARE(sent.size(), 0);
        f.controller.selectTopic(topicId); QTRY_VERIFY(!f.controller.busy());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.draft(), QString{}, 2000);
        QVERIFY(f.controller.encryption().value("pendingSend").toMap().isEmpty());
        QCOMPARE(f.posts, 2); QCOMPARE(sent.size(), 0);
    }

    void encryptedUnicodeWireBoundaryPreservesValidPointsAndRejectsOversizedEnvelope_data() {
        QTest::addColumn<int>("points"); QTest::addColumn<bool>("allowed");
        QTest::newRow("astral-39988-fits") << 39988 << true;
        QTest::newRow("astral-40000-wire-overflow") << 40000 << false;
    }
    void encryptedUnicodeWireBoundaryPreservesValidPointsAndRejectsOversizedEnvelope() {
        QFETCH(int, points); QFETCH(bool, allowed);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const char32_t scalar = 0x1f642; const auto text = QString::fromUcs4(&scalar, 1).repeated(points);
        SourceSendPeer peer; QVERIFY(peer.prepare(f, topicId, text));
        f.controller.setDraft(text); QTRY_VERIFY(f.controller.canSend());
        QSignalSpy sent(&f.controller, &NativeController::messageSent); f.controller.send();
        if (allowed) {
            QTRY_COMPARE(sent.size(), 1); QCOMPARE(f.posts, 2);
            QCOMPARE(f.requestBody.value("payload").toObject().value("content").toString().size(), 213336);
            QVERIFY(peer.verify(f.requestBody.value("payload").toObject(), topicId).value("matchesExpected").toBool());
        } else {
            QCOMPARE(f.posts, 1); QCOMPARE(sent.size(), 0); QVERIFY(f.controller.draft() == text);
            QVERIFY(!f.controller.error().isEmpty()); QVERIFY(f.controller.encryption().value("pendingSend").toMap().isEmpty());
        }
    }

    void currentSendingKeyCanBeAvailableWithoutAnyLoadedMessageForThatGeneration() {
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start()); QVERIFY(f.prepareA());
        f.stream["current_encryption_key_uuid"] = OtherGeneration;
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject(); payload["kind"] = "stream.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event)); QTRY_VERIFY(f.controller.canCompose());
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
        f.controller.requestChatKey(OtherGeneration); QTRY_COMPARE(f.posts, 2);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QTRY_COMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), OtherGeneration);
        QVERIFY(f.controller.canCompose());
        for (const auto &raw : f.controller.messages())
            QVERIFY(raw.toMap().value("encryptionKeyUuid").toString() != OtherGeneration);
        const auto requests = f.http.requests.size();
        for (int i = 0; i < 100; ++i) {
            QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), OtherGeneration);
            QVERIFY(f.controller.canCompose());
        }
        QCOMPARE(f.http.requests.size(), requests);
    }

    void sourcePeerLegacyOneShotPreservesReadAndReceipt() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        f.controller.requestChatKey(returnedId);
        QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        const auto source = f.sourceResponse();
        QVERIFY(source.value("ok").toBool());
        QVERIFY(!source.contains("readyForNativeSend"));
        QCOMPARE(source.keys(), QStringList({"encrypted", "message", "ok"}));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        f.cipher["payload"] = source.value("encrypted");
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Synthetic source-granted plaintext"));
        const auto receipt = f.vault.requestCompletionByMessage(f.vault.lease(), {streamId, returnedId},
            f.savedRequest.value("uuid").toString());
        QVERIFY(receipt.value); QCOMPARE(receipt.value->outcome, NativeChatKeyVault::Outcome::Granted);
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        QVERIFY(!f.controller.canSend()); // No draft; this assertion also survives future send support.
    }

    void sourcePeerOptionalModeKeepsGrantAndDecryptInOneProcess() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        SourceSendPeer peer;
        const QString text = "Synthetic two-line source peer control";
        QVERIFY(peer.prepare(f, secondTopicId, text));
        const auto result = peer.verify(peer.source.value("encrypted").toObject(), secondTopicId);
        QVERIFY(result.value("ok").toBool()); QVERIFY(result.value("matchesExpected").toBool());
        QCOMPARE(result.value("plaintextBytes").toInt(), text.toUtf8().size());
        QCOMPARE(result.value("keyUuid").toString(), returnedId);
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        QVERIFY(!f.controller.canSend());
    }

    void encryptedTextSendUsesActualGrantedKeyAndFrozenTarget_data() {
        QTest::addColumn<bool>("aggregate");
        QTest::newRow("selected-topic") << false;
        QTest::newRow("stream-aggregate") << true;
    }
    void encryptedTextSendUsesActualGrantedKeyAndFrozenTarget() {
        QFETCH(bool, aggregate);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString target = aggregate ? secondTopicId : topicId;
        const QString text = QString::fromUtf8("Synthetic native encrypted send — kept inside the fixture");
        SourceSendPeer peer; QVERIFY(peer.prepare(f, target, text));
        const auto requestPublicId = f.savedRequest.value("uuid").toString();
        if (aggregate) {
            f.controller.selectStream(streamId);
            QTRY_VERIFY(!f.controller.busy() && f.controller.topics().size() == 2 && f.controller.messages().size() == 1);
            QCOMPARE(f.controller.selectedTopic(), QString{});
            QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        }
        const auto postsBefore = f.posts, catalogBefore = f.catalogGets;
        const auto topicsBefore = f.topicGets, streamBefore = f.streamGets, bindingsBefore = f.bindingGets;
        const auto historyBefore = exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/");
        const auto usersBefore = exactRequests(f, "GET", "/api/workspace/v1/users/");
        QSignalSpy confirmed(&f.controller, &NativeController::messageSent);
        f.controller.setDraft(text);
        // Independent capability boundary: aggregate must retain its own draft.
        QCOMPARE(f.controller.draft(), text);
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.canSend(), 2000);
        f.controller.send();
        QTRY_COMPARE(f.posts, postsBefore + 1);
        const auto body = f.requestBody, payload = body.value("payload").toObject();
        const auto localId = body.value("uuid").toString();
        QVERIFY(isUuid(localId)); QCOMPARE(body.value("stream_uuid").toString(), streamId);
        QCOMPARE(body.value("topic_uuid").toString(), target);
        QCOMPARE(payload.keys(), QStringList({"content", "key_uuid", "kind"}));
        QCOMPARE(payload.value("kind").toString(), QString("e2ee"));
        QCOMPARE(payload.value("key_uuid").toString(), returnedId);
        const auto encoded = payload.value("content").toString().toLatin1();
        const auto envelope = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        QVERIFY(envelope.size() >= 48); QVERIFY(encoded.size() <= 213336);
        QVERIFY(envelope.toBase64() == encoded);
        QVERIFY(!QJsonDocument(body).toJson(QJsonDocument::Compact).contains(text.toUtf8()));
        const auto publicId = QUuid::createUuidV5(QUuid(target), localId.toUtf8()).toString(QUuid::WithoutBraces);
        QTRY_COMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed.first().at(0).toString(), publicId);
        QCOMPARE(confirmed.first().at(1).toString(), streamId);
        QCOMPARE(confirmed.first().at(2).toString(), target);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 1);
        const auto saved = f.savedRequests.value(publicId);
        QCOMPARE(saved.value("uuid").toString(), publicId); QCOMPARE(saved.value("user_uuid").toString(), userId);
        QCOMPARE(saved.value("author_uuid").toString(), userId); QVERIFY(saved.value("is_own").toBool());
        QVERIFY(saved.value("payload").toObject() == payload);
        const auto verified = peer.verify(payload, target);
        QVERIFY(verified.value("ok").toBool()); QVERIFY(verified.value("matchesExpected").toBool());
        QCOMPARE(verified.value("plaintextBytes").toInt(), text.toUtf8().size());
        QCOMPARE(verified.value("keyUuid").toString(), returnedId);
        QCOMPARE(f.controller.draft(), QString{});
        QCOMPARE(f.controller.selectedStream(), streamId);
        QCOMPARE(f.controller.selectedTopic(), aggregate ? QString{} : topicId);
        QCOMPARE(f.posts, postsBefore + 1); QCOMPARE(f.catalogGets, catalogBefore);
        QCOMPARE(f.topicGets, topicsBefore); QCOMPARE(f.streamGets, streamBefore); QCOMPARE(f.bindingGets, bindingsBefore);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/"), historyBefore);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/users/"), usersBefore);
        const auto receipt = f.vault.requestCompletionByMessage(f.vault.lease(), {streamId, returnedId},
            requestPublicId);
        QVERIFY(receipt.value); QCOMPARE(receipt.value->outcome, NativeChatKeyVault::Outcome::Granted);
    }

    void encryptedTextSendBlockedWithoutSelectedCurrentKey_data() {
        QTest::addColumn<bool>("nullCurrent");
        QTest::newRow("missing-current") << false;
        QTest::newRow("null-current-readable-history") << true;
    }
    void encryptedTextSendBlockedWithoutSelectedCurrentKey() {
        QFETCH(bool, nullCurrent);
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        if (nullCurrent) {
            QVERIFY(f.prepareA());
            f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
            QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
            QCOMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Synthetic source-granted plaintext"));
        }
        const auto before = f.posts;
        f.controller.setDraft("Synthetic blocked encrypted draft");
        QCOMPARE(f.controller.draft(), QString("Synthetic blocked encrypted draft"));
        QVERIFY(!f.controller.canSend()); f.controller.send();
        QCOMPARE(f.posts, before);
        QVERIFY(!f.controller.draft().isEmpty());
        for (const auto &request : f.requestBodies)
            QVERIFY(request.value("payload").toObject().value("kind").toString() != "e2ee");
    }


    void ordinaryAccountAuthorityOnlyRunsForActualServiceWork_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("plaintext-with-vault-no-authority") << QString("plain-vault");
        QTest::newRow("plaintext-without-vault-no-authority") << QString("plain-no-vault");
        QTest::newRow("owned-request-with-vault-positive-authority") << QString("request-vault");
    }
    void ordinaryAccountAuthorityOnlyRunsForActualServiceWork()
    {
        QFETCH(QString, mode);
        const bool withVault = mode != "plain-no-vault", withRequest = mode == "request-vault";
        Fixture f; f.stream = fullEncryptedStream(); f.stream["encryption"] = false;
        f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        f.cipher = message("Existing ordinary history"); useCompleteSendEcho(f);
        if (withRequest) {
            // A real loaded typed own/other-device R stays visible before Vault
            // attachment. This control checks preparation, not HPKE issuance.
            f.cipher["payload"] = QJsonObject{{"kind", "key_request"}, {"key_uuid", returnedId},
                {"device_id", "Synthetic other device"}, {"device_name", "Synthetic request witness"},
                {"public_key", QString::fromLatin1(QByteArray(32, 'p').toBase64())}};
            QVERIFY(parseMessage(f.cipher, projectId));
        }
        auto held = std::make_shared<std::function<void()>>();
        auto acknowledged = std::make_shared<int>(0);
        const auto previous = f.http.handler;
        f.http.handler = [previous, held, acknowledged](const Request &wire) {
            if (wire.method == "GET" && wire.target.path() == "/ordinary-authority-barrier")
                return Response{QJsonObject{{"ok", true}}};
            auto response = previous(wire); // actual fixture counters acknowledge the wire first
            if (wire.method == "GET" && wire.target.path() == "/api/workspace/v1/messenger/streams/" + streamId)
                response.defer = [held, acknowledged](std::function<void()> send) {
                    ++*acknowledged; if (!*held) *held = std::move(send);
                };
            return response;
        };
        const auto restore = qScopeGuard([&] {
            f.http.handler = previous;
            if (*held) { auto once = std::move(*held); *held = {}; once(); }
        });
        // Exact existing successful Fixture.start setup, split only to provide
        // the no-Vault control. Existing setup budgets remain 2000ms.
        f.controller.login(f.tls.origin().toString(), "Synthetic", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&] { return f.controller.projects().size() == 1; }, 2000));
        f.controller.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return f.controller.streams().size() == 1; }, 2000));
        f.controller.selectStream(streamId);
        QVERIFY(QTest::qWaitFor([&] { return f.controller.topics().size() == 2; }, 2000));
        f.controller.selectTopic(topicId);
        QVERIFY(QTest::qWaitFor([&] { return !f.controller.busy() && f.controller.messages().size() == 1; }, 2000));
        if (withVault) {
            auto opened = std::make_shared<int>(0);
            f.vault.open(f.tls.origin(), projectId, userId, f.deviceName,
                [opened](auto failure) { *opened = failure == NativeChatKeyVault::Failure::None ? 1 : -1; });
            QVERIFY(QTest::qWaitFor([opened] { return *opened != 0; }, 2000));
            QCOMPARE(*opened, 1); f.controller.useChatKeys(&f.vault);
        }
        // Two fixed real transport round trips drive already-queued Qt work.
        // If the old producer fires, positively witness and release its exact
        // acknowledged detail, then finish its actual topics/bindings chain.
        // This is not a sleep or a retry-until-zero loop.
        const auto drain = [&] {
            for (int pass = 0; pass < 2; ++pass) {
                auto done = std::make_shared<int>(0);
                const QPointer<ApiClient> api(&f.api);
                QTimer::singleShot(0, &f.api, [api, done] {
                    if (api) api->request("GET", "/ordinary-authority-barrier", {}, {},
                        [done](const ApiResult &result) { *done = result.ok() ? 1 : -1; });
                });
                if (!QTest::qWaitFor([done] { return *done != 0; }, 2000) || *done != 1) return false;
                if (*held) {
                    bool active = false;
                    for (auto *reply : f.api.findChildren<QNetworkReply *>())
                        if (!reply->isFinished()
                            && reply->url().path() == "/api/workspace/v1/messenger/streams/" + streamId
                            && reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "GET")
                            active = true;
                    if (!active || *acknowledged != 1 || f.streamGets != 1) return false;
                    auto once = std::move(*held); *held = {}; once();
                    if (!QTest::qWaitFor([&] { return f.bindingGets == 1; }, 2000)) return false;
                }
            }
            return QTest::qWaitFor([&] { return f.api.findChildren<QNetworkReply *>().isEmpty(); }, 2000);
        };
        QVERIFY(drain());
        QCOMPARE(f.posts, 0);
        if (withRequest) {
            QCOMPARE(*acknowledged, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
            const auto row = incomingKeyRequestRow(f.controller, messageId);
            QCOMPARE(row.value("keyUuid").toString(), returnedId);
            QCOMPARE(row.value("status").toString(), QString("pending"));
            QVERIFY(!row.value("hasRequestedKey").toBool());
            QVERIFY(!row.value("canGrant").toBool()); QVERIFY(!row.value("canReject").toBool());
            return;
        }
        QVERIFY(f.controller.encryption().value("keyRequests").toList().isEmpty());
        QCOMPARE(f.bindingGets, 0); QCOMPARE(f.streamGets, 0); QCOMPARE(*acknowledged, 0);
        const auto topics = f.topicGets;
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        const QString text = "Synthetic ordinary send without authority overhead";
        f.controller.setDraft(text); QVERIFY(f.controller.canSend()); f.controller.send();
        QTRY_COMPARE(sent.size(), 1); QCOMPARE(f.posts, 1);
        QCOMPARE(f.requestBody.value("topic_uuid").toString(), topicId);
        QCOMPARE(f.requestBody.value("payload").toObject(),
            (QJsonObject{{"kind", "markdown"}, {"content", text}}));
        const auto canonical = f.requestBody.value("uuid").toString(); QVERIFY(isUuid(canonical));
        const auto publicId = QUuid::createUuidV5(QUuid(topicId), canonical.toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(sent.first().at(0).toString(), publicId);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 1);
        QCOMPARE(f.controller.draft(), QString{});
        QVERIFY(drain());
        QCOMPARE(f.bindingGets, 0); QCOMPARE(f.streamGets, 0); QCOMPARE(f.topicGets, topics);
    }


    void ordinaryRetainedRejectRecoversAuthorityAfterSameRouteHistoryOmissionAndWssTrustReset()
    {
        OrdinaryRejectCase c;
        RealtimeTransport transport(c.f); // destroyed before the fixture it references
        QVERIFY(c.start()); QVERIFY(transport.start());
        // The initial real live snapshot may replace the first loaded window.
        // Re-publish the same actual Source R through WSS, not a private fact.
        transport.publish(11, "message.created", c.peer.requestDto);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(11));
        QTRY_VERIFY(c.row().value("canReject").toBool());
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.f.posts, 0);
        UncertainIssuerPost uncertain(c.f, false);
        c.f.controller.rejectChatKey(c.peer.requestId);
        QTRY_VERIFY(uncertain.failed);
        QTRY_COMPARE(c.response().value("phase").toString(), QString("uncertain"));
        const auto frozen = uncertain.frozen, canonical = uncertain.canonical;
        const auto publicId = canonical.value("uuid").toString();
        const auto publicPath = "/api/workspace/v1/messenger/messages/" + publicId;
        QVERIFY(!c.f.savedRequests.contains(publicId));
        QCOMPARE(c.f.posts, 1); QVERIFY(c.response().value("canCheck").toBool());
        QVERIFY(c.response().value("canRetry").toBool());
        const auto owner = c.f.controller.ownerKey(); const auto generation = c.f.api.generation();
        const auto lease = c.f.vault.lease();
        const auto catalogs = c.f.catalogGets, snapshots = transport.snapshots;
        const auto history = c.f.controller.historyCommitRevision();
        const auto historyGets = [&] {
            int count = 0;
            for (const auto &wire : c.f.http.requests)
                if (wire.method == "GET" && wire.target.path() == "/api/workspace/v1/messenger/messages/"
                    && QUrlQuery(wire.target).queryItemValue("stream_uuid") == streamId
                    && QUrlQuery(wire.target).queryItemValue("topic_uuid") == topicId
                    && !QUrlQuery(wire.target).hasQueryItem("uuid")) ++count;
            return count;
        };
        const auto beforeHistoryGets = historyGets();
        // Public same-id reselect performs an accepted history replacement.
        // It advances the selection ticket, but route values/catalog/API/Vault
        // are unchanged and it is NOT canonical message deletion or access loss.
        c.f.controller.selectTopic(topicId);
        QTRY_COMPARE(historyGets(), beforeHistoryGets + 1);
        QTRY_VERIFY(c.f.controller.historyCommitRevision() > history);
        QTRY_VERIFY(!c.f.controller.busy() && c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.f.controller.selectedStream(), streamId);
        QCOMPARE(c.f.controller.selectedTopic(), topicId);
        QVERIFY(displayed(c.f.controller, c.peer.requestId).isEmpty());
        QCOMPARE(c.f.controller.messages().size(), 1);
        QCOMPARE(c.f.controller.messages().first().toMap().value("kind").toString(), QString("markdown"));
        QCOMPARE(c.row().value("keyUuid").toString(), OtherGeneration);
        QCOMPARE(c.row().value("status").toString(), QString("pending"));
        QVERIFY(c.response().value("canRetry").toBool());
        const auto details = c.f.streamGets, bindings = c.f.bindingGets;
        const auto acceptedHistory = c.f.controller.historyCommitRevision();
        const auto eventGets = transport.eventGets;
        c.f.streamDelay = 1200; transport.holdReady = true;
        for (auto &socket : transport.sockets) if (socket) socket->close();
        QTRY_VERIFY(c.f.controller.syncState() != "live");
        QTRY_VERIFY(transport.eventGets > eventGets);
        QTRY_VERIFY(!transport.pendingReady.isEmpty());
        QVERIFY(!c.response().value("canRetry").toBool());
        QCOMPARE(c.f.posts, 1);
        QCOMPARE(c.f.streamGets, details); QCOMPARE(c.f.bindingGets, bindings);
        transport.releaseReady();
        QTRY_COMPARE(c.f.controller.syncState(), QString("live"));
        // This is the discriminating edge: empty controls still have an owned
        // unfinished response whose Retry admission needs current authority.
        QTRY_COMPARE(c.f.streamGets, details + 1);
        QTRY_VERIFY(c.pending("GET", "/api/workspace/v1/messenger/streams/" + streamId));
        QCOMPARE(c.f.bindingGets, bindings);
        QCOMPARE(c.f.posts, 1); QVERIFY(!c.response().value("canRetry").toBool());
        // Exact public status read does not depend on group A or fresh roles
        // and does not itself run authority renewal after a 404.
        const auto statusGets = exactRequests(c.f, "GET", publicPath);
        QVERIFY(c.response().value("canCheck").toBool());
        c.f.controller.checkChatKeyResponse(c.peer.requestId);
        QTRY_COMPARE(exactRequests(c.f, "GET", publicPath), statusGets + 1);
        QTRY_VERIFY(!c.response().value("busy").toBool());
        QCOMPARE(c.response().value("phase").toString(), QString("uncertain"));
        QCOMPARE(c.f.posts, 1);
        QTRY_COMPARE(c.f.bindingGets, bindings + 1);
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_VERIFY(c.response().value("canRetry").toBool());
        QCOMPARE(c.f.controller.ownerKey(), owner); QCOMPARE(c.f.api.generation(), generation);
        QCOMPARE(c.f.vault.lease().owner, lease.owner); QCOMPARE(c.f.vault.lease().device, lease.device);
        QCOMPARE(c.f.vault.lease().issuer, lease.issuer); QCOMPARE(c.f.vault.lease().generation, lease.generation);
        QCOMPARE(c.f.catalogGets, catalogs); QCOMPARE(transport.snapshots, snapshots);
        QCOMPARE(c.f.controller.historyCommitRevision(), acceptedHistory);
        QCOMPARE(c.f.controller.selectedStream(), streamId); QCOMPARE(c.f.controller.selectedTopic(), topicId);
        QVERIFY(displayed(c.f.controller, c.peer.requestId).isEmpty());
        // Explicit Retry still renews its own authority/request and sends only
        // the exact original wire after a fresh public 404.
        c.f.streamDelay = 0;
        const auto retryDetails = c.f.streamGets, retryBindings = c.f.bindingGets;
        const auto requestGets = exactRequests(c.f, "GET", "/api/workspace/v1/messenger/messages/" + c.peer.requestId);
        c.f.controller.retryChatKeyResponse(c.peer.requestId);
        QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
        QCOMPARE(c.f.posts, 2); QCOMPARE(c.f.requestBody, frozen);
        QCOMPARE(c.f.streamGets, retryDetails + 1); QCOMPARE(c.f.bindingGets, retryBindings + 1);
        QCOMPARE(exactRequests(c.f, "GET", "/api/workspace/v1/messenger/messages/" + c.peer.requestId), requestGets + 1);
        QCOMPARE(c.f.requestBody.value("topic_uuid").toString(), secondTopicId);
        QCOMPARE(c.f.requestBody.value("payload").toObject().value("kind").toString(), QString("key_reject"));
        const auto result = c.peer.finish(c.f.savedRequest, frozen.value("uuid").toString());
        QVERIFY(result.value("ok").toBool()); QCOMPARE(result.value("completionOutcome").toString(), QString("rejected"));
        QCOMPARE(result.value("completionRequest").toString(), c.peer.requestId);
        QCOMPARE(result.value("completionResponse").toString(), publicId);
        QCOMPARE(result.value("recipientPending").toInt(), 0); QVERIFY(!result.value("recipientHasKey").toBool());
    }

    void ordinaryTextSendRemainsMarkdownAndAggregateStillPrompt_data() {
        QTest::addColumn<bool>("aggregate");
        QTest::newRow("selected-topic") << false;
        QTest::newRow("stream-aggregate") << true;
    }
    void ordinaryTextSendRemainsMarkdownAndAggregateStillPrompt() {
        QFETCH(bool, aggregate);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["encryption"] = false;
        f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        f.cipher = message("Existing ordinary history"); useCompleteSendEcho(f); QVERIFY(f.start());
        if (aggregate) {
            f.controller.selectStream(streamId);
            QTRY_VERIFY(!f.controller.busy() && f.controller.topics().size() == 2 && f.controller.messages().size() == 1);
        }
        const QString text = "Synthetic ordinary send control";
        f.controller.setDraft(text);
        if (aggregate) {
            QCOMPARE(f.controller.selectedTopic(), QString{}); QCOMPARE(f.controller.draft(), QString{});
            QVERIFY(!f.controller.canSend()); f.controller.send(); QCOMPARE(f.posts, 0);
            return;
        }
        QCOMPARE(f.controller.draft(), text); QVERIFY(f.controller.canSend());
        QSignalSpy confirmed(&f.controller, &NativeController::messageSent);
        f.controller.send(); QTRY_COMPARE(confirmed.size(), 1); QCOMPARE(f.posts, 1);
        QCOMPARE(f.requestBody.value("topic_uuid").toString(), topicId);
        QVERIFY(f.requestBody.value("payload").toObject() == QJsonObject({{"kind", "markdown"}, {"content", text}}));
        const auto localId = f.requestBody.value("uuid").toString(); QVERIFY(isUuid(localId));
        const auto publicId = QUuid::createUuidV5(QUuid(topicId), localId.toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(confirmed.first().at(0).toString(), publicId);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 1);
        QCOMPARE(f.controller.draft(), QString{}); QCOMPARE(f.bindingGets, 0);
    }

#ifdef WORKSPACE_NATIVE_E2EE_UI
    // Additive UI capability rows. Core invokable is never called directly here.
    void uiOrdinaryAccountRecipientIsPublishedBeforeAnyServiceBodyMount_data()
    {
        QTest::addColumn<bool>("external");QTest::addColumn<bool>("grant");
        QTest::newRow("native-no-vault-foreign-grant")<<false<<true;
        QTest::newRow("native-no-vault-foreign-reject")<<false<<false;
        QTest::newRow("external-no-vault-foreign-reject")<<true<<false;
    }
    void uiOrdinaryAccountRecipientIsPublishedBeforeAnyServiceBodyMount()
    {
        QFETCH(bool,external);QFETCH(bool,grant);AccountRequestCase a;auto &c=a.c;
        c.f.stream["role"]="guest";c.f.issuerRole="moderator";
        if(external) {
            c.f.stream["source_name"]="zulip";
            c.f.stream["provider_uuid"]="eeeeeeee-eeee-4eee-8eee-000000000010";
            c.f.stream["provider_identity"]="synthetic-account-provider";
        }
        if(grant) {
            auto payload=c.f.cipher.value("payload").toObject();payload["kind"]="key_grant";
            payload["content"]=QString::fromLatin1(QByteArray(48,'g').toBase64());c.f.cipher["payload"]=payload;
        }
        // Only these new UI3 DTOs: enrich the ACTUAL served/history objects,
        // never normalize a sparse DTO after export or change Core/default14.
        const auto canonical=[](QJsonObject &row) {
            row["user_uuid"]=userId;row["pinned"]=false;
            row["is_own"]=row.value("author_uuid").toString()==userId;
            row["reactions"]=QJsonObject{};row["reaction_users"]=QJsonObject{};
            row["updated_at"]=row.value("created_at");
        };
        canonical(c.request);canonical(c.f.cipher);c.lookupRows=QJsonArray{c.request};
        for(const auto &row:QJsonArray{c.request,c.f.cipher}) {
            const auto dto=row.toObject();
            for(const auto *field:{"user_uuid","created_at","updated_at"})QVERIFY(dto.value(field).isString()&&!dto.value(field).toString().isEmpty());
            for(const auto *field:{"read","pinned","starred","is_own"})QVERIFY(dto.value(field).isBool());
            QVERIFY(dto.value("reactions").isObject()&&dto.value("reaction_users").isObject());
            QCOMPARE(dto.value("user_uuid").toString(),userId);QVERIFY(parseMessage(dto,projectId));
        }
        QCOMPARE(c.lookupRows,QJsonValue(QJsonArray{c.request}));
        c.lookupDelay=0; // The response is held by a real transport gate instead of a timer.
        AccountUiResponseGate gate(c.f,c.requestId);QVERIFY(gate.available());
        AccountPublicCapture capture(c); // passive, after the existing delivery gate; destructs first
        QVERIFY(parseMessage(c.request,projectId));QVERIFY(parseMessage(c.f.cipher,projectId));
        QVERIFY(a.start());QVERIFY(!c.f.vault.available());
        QTRY_COMPARE(c.lookupCount,1);QTRY_VERIFY(gate.acknowledgedAndHeld());QVERIFY(c.pending());
        QCOMPARE(gate.matchingRequests(),1);QVERIFY(c.exactQuery);
        const auto raw=displayed(c.f.controller,messageId);
        QCOMPARE(raw.value("kind").toString(),grant?QString("key_grant"):QString("key_reject"));
        QCOMPARE(raw.value("authorId").toString(),Issuer);QVERIFY(!raw.value("own").toBool());
        QVERIFY(!c.metadata().value("recipientKnown").toBool());
        QVERIFY(c.f.controller.encryption().value("keyRequests").toList().isEmpty());
        QCOMPARE(c.f.controller.messages().size(),1);QVERIFY(displayed(c.f.controller,c.requestId).isEmpty());
        NativePreferences preferences(c.f.directory.filePath("ordinary-account-prefilter-ui.ini"));
        preferences.setOwner(c.f.controller.ownerKey());preferences.setLocale("en");preferences.setMode("light");
        QQmlApplicationEngine engine;loadNativeUi(engine,&c.f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {
            if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Ordinary account window did not close",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QSignalSpy frame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!frame.isEmpty());
        auto *list=encryptedUiItem(window,"messageList");QVERIFY(list);QCOMPARE(list->property("count").toInt(),0);
        QVERIFY(serviceUiProperty(list,"serviceGroups").toList().isEmpty());
        QVERIFY(!encryptedUiItem(window,"serviceGroupToggle-"+messageId));
        QVERIFY(!encryptedUiItem(window,"serviceBodyLoader-"+messageId));
        QVERIFY(!encryptedUiItem(window,"keyServiceResponse-"+messageId));
        // Match Source's initialized-empty/live-enabled context before R publication.
        QTRY_VERIFY(c.f.controller.property("messageHistoryReady").toBool());
        QTRY_VERIFY(serviceUiProperty(list,"serviceSession").toMap().value("ready").toBool());
        QVERIFY(serviceUiProperty(list,"serviceLiveEnabled").toBool());
        const auto initialSession=serviceUiProperty(list,"serviceSession").toMap();
        QVERIFY(initialSession.value("seen").toMap().isEmpty());
        QVERIFY(initialSession.value("preferences").toMap().isEmpty());QVERIFY(initialSession.value("latest").isNull());
        QVERIFY(!serviceUiProperty(list,"serviceSelectionMode").toBool());
        const auto historyRevision=c.f.controller.property("historyCommitRevision").toULongLong();
        capture.recordInitial(initialSession,serviceUiProperty(list,"serviceLiveEnabled").toBool(),
            serviceUiProperty(list,"serviceSelectionMode").toBool(),historyRevision);QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(c.lookupCount,1);QVERIFY(c.pending());QVERIFY(gate.acknowledgedAndHeld());
        QSignalSpy published(&c.f.controller,&NativeController::messagesChanged);QVERIFY(published.isValid());
        QVERIFY(gate.release());
        // Poll actual Main only: no controller.messages()/metadata getter is used
        // to provoke a rebuild before the visible publication assertion.
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+messageId));
        QVERIFY(!published.isEmpty());
        auto *header=encryptedUiItem(window,"serviceGroupToggle-"+messageId);QVERIFY(header);
        // Relevance publication is not another history commit: first unseen
        // group in an already-ready empty session auto-opens in pinned Source.
        QTRY_VERIFY(header->property("expanded").toBool());QCOMPARE(list->property("count").toInt(),1);
        QCOMPARE(c.f.controller.property("historyCommitRevision").toULongLong(),historyRevision);
        const auto groups=serviceUiProperty(list,"serviceGroups").toList();QCOMPARE(groups.size(),1);
        const auto ids=groups.first().toMap().value("ids").toList();QCOMPARE(ids.size(),1);QCOMPARE(ids.first().toString(),messageId);
        auto *loader=encryptedUiItem(window,"serviceBodyLoader-"+messageId);QVERIFY(loader);
        QTRY_VERIFY(loader->property("active").toBool());QTRY_VERIFY(loader->property("item").value<QObject *>());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+messageId));
        capture.recordExpanded(window,external,grant);QVERIFY(!QTest::currentTestFailed());
        // One real keyboard action proves collapse/unmount without an R retry.
        keyMissingRequestDisclosure(window,header,Qt::Key_Space);QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(!header->property("expanded").toBool());
        QTRY_VERIFY(!loader->property("active").toBool());QTRY_VERIFY(!loader->property("item").value<QObject *>());
        QVERIFY(!encryptedUiItem(window,"keyServiceResponse-"+messageId));
        QVERIFY(!encryptedUiItem(window,"requestRejectedChatKeyButton-"+messageId));
        QTRY_VERIFY(!c.pending());QCOMPARE(c.lookupCount,1);QCOMPARE(gate.matchingRequests(),1);
        const auto facts=c.metadata();QVERIFY(facts.value("recipientKnown").toBool());
        QCOMPARE(facts.value("recipientId").toString(),userId);QVERIFY(!facts.value("ownCurrentDevice").toBool());
        QVERIFY(!facts.value("hasKey").toBool());QVERIFY(!facts.value("offerRequest").toBool());QVERIFY(!facts.value("canRequest").toBool());
        QVERIFY(!c.f.vault.available());QVERIFY(c.f.controller.encryption().value("keyRequests").toList().isEmpty());
        QCOMPARE(c.f.controller.messages().size(),1);QVERIFY(displayed(c.f.controller,c.requestId).isEmpty());
        QCOMPARE(c.f.posts,0);QCOMPARE(c.f.streamGets,0);QCOMPARE(c.f.bindingGets,0);
        capture.writeFinal(window,external,grant,historyRevision);QVERIFY(!QTest::currentTestFailed());
    }

    void uiMissingRequestKeyboardDisclosureHydratesTheActualSourceResponse_data()
    {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("grant");
        QTest::newRow("en-light-grant") << QString("en") << QString("light") << true;
        QTest::newRow("en-light-reject") << QString("en") << QString("light") << false;
        QTest::newRow("ru-dark-grant") << QString("ru") << QString("dark") << true;
        QTest::newRow("ru-dark-reject") << QString("ru") << QString("dark") << false;
    }
    void uiMissingRequestKeyboardDisclosureHydratesTheActualSourceResponse()
    {
        QFETCH(QString, locale); QFETCH(QString, mode); QFETCH(bool, grant);
        MissingRequestCase c; MissingRequestPublicCapture capture(c);
        QVERIFY(c.hasApi()); QVERIFY(prepareOwnMissingRequestUi(c, grant));
        QVERIFY(c.request.value("reaction_users").isObject());
        QVERIFY(c.f.cipher.value("reaction_users").isObject());
        for(const auto &dto:{c.request,c.f.cipher}) {
            QVERIFY(dto.value("pinned").isBool()); QVERIFY(!dto.value("pinned").toBool());
            QVERIFY(dto.value("reactions").isObject()); QVERIFY(dto.value("reactions").toObject().isEmpty());
            QVERIFY(dto.value("updated_at").isString()); QVERIFY(!dto.value("updated_at").toString().isEmpty());
            QCOMPARE(dto.value("updated_at"),dto.value("created_at"));
        }
        const auto id = c.f.cipher.value("uuid").toString(); QVERIFY(!c.metadata(id).value("recipientKnown").toBool());
        QCOMPARE(c.f.controller.messages().size(), 1); QVERIFY(displayed(c.f.controller, c.requestId).isEmpty());
        NativePreferences preferences(c.f.directory.filePath("missing-request-disclosure.ini"));
        preferences.setOwner(c.f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &c.f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([window=QPointer<QQuickWindow>(window)] { if(!closeEncryptedVisualWindow(window.data())) QTest::qFail("Missing-R UI did not close",__FILE__,__LINE__); });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *header = encryptedUiItem(window, "serviceGroupToggle-"+id); QVERIFY(header); QVERIFY(!header->property("expanded").toBool());
        auto *loader = encryptedUiItem(window, "serviceBodyLoader-"+id); QVERIFY(loader);
        QVERIFY(!loader->property("active").toBool()); QVERIFY(!loader->property("item").value<QObject *>());
        for(int n=0;n<20;++n) { (void)c.f.controller.messages(); (void)c.f.controller.encryption(); }
        QCOMPARE(c.lookupCount,0); // History, getter reads and collapsed render are not demand.
        const auto posts=c.f.posts, details=c.f.streamGets, bindings=c.f.bindingGets, topics=c.f.topicGets;
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed());
        QTRY_COMPARE(c.lookupCount,1); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount,1); QVERIFY(c.exactQuery);
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+id));
        QVERIFY(!c.metadata(id).value("recipientKnown").toBool());
        auto *title=encryptedUiItem(window,"keyServiceResponseText-"+id); QVERIFY(title);
        QVERIFY(title->property("text").toString().contains(locale=="ru"?QString("Информация о получателе недоступна."):QString("Recipient information is unavailable.")));
        const auto expected=missingRequestUiExpectedTitle(c,grant,true,locale);
        QTRY_COMPARE(missingRequestUiTitle(window,id),expected);
        QTRY_VERIFY(!c.pending()); QVERIFY(c.flush()); const auto body=c.diskBody(); QVERIFY(body);
        auto expectedWorkerBody=c.request; expectedWorkerBody.remove("pinned"); expectedWorkerBody.remove("updated_at");
        QVERIFY(!body->row.contains("pinned")); QVERIFY(!body->row.contains("updated_at"));
        QCOMPARE(body->row.value("payload"),c.request.value("payload"));
        QCOMPARE(body->row,expectedWorkerBody);
        const auto facts=c.metadata(id); QVERIFY(facts.value("recipientKnown").toBool());
        QCOMPARE(facts.value("recipientId").toString(),userId); QVERIFY(facts.value("ownCurrentDevice").toBool());
        QCOMPARE(facts.value("keyUuid").toString(),returnedId);
        QCOMPARE(header->property("status").toString(),grant?QString{}:QString("action-required"));
        QCOMPARE(encryptedUiItem(window,"keyServiceResponseText-"+id)->property("textFormat").toInt(),int(Qt::PlainText));
        QCOMPARE(c.f.controller.messages().size(),1); QVERIFY(displayed(c.f.controller,c.requestId).isEmpty());
        QCOMPARE(c.f.posts,posts); QCOMPARE(c.lookupCount,1); QCOMPARE(c.f.streamGets,details); QCOMPARE(c.f.bindingGets,bindings); QCOMPARE(c.f.topicGets,topics);
        capture.write(window,locale,mode,grant,body->row); QVERIFY(!QTest::currentTestFailed());
    }

    void uiMissingRequestSharedRSurvivesFirstConsumerDeletionAndCollapse()
    {
        MixedOutcomeCase source; QVERIFY(source.start()); MissingRequestCase c; QVERIFY(c.hasApi());
        QVERIFY(prepareExternalMissingRequestUi(c,source,true));
        const auto first=source.grant.value("uuid").toString(), second=c.f.cipher.value("uuid").toString();
        NativePreferences preferences(c.f.directory.filePath("missing-request-shared.ini")); preferences.setOwner(c.f.controller.ownerKey());
        QQmlApplicationEngine engine; loadNativeUi(engine,&c.f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Shared missing-R UI did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *header=encryptedUiItem(window,"serviceGroupToggle-"+first); QVERIFY(header); QCOMPARE(c.lookupCount,0);
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+first));
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+second));
        QTRY_COMPARE(c.lookupCount,1); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount,1); // Both mounted consumers have demanded the same R.
        auto firstDto=source.grant; firstDto["user_uuid"]=userId; firstDto["topic_uuid"]=topicId;
        firstDto["created_at"]="2026-01-02T10:01:00.000Z";
        auto removal=cryptoEvent(firstDto); auto payload=removal.value("payload").toObject();
        payload["kind"]="message.deleted"; removal["payload"]=payload;
        QVERIFY(c.f.controller.applyRealtimeEvent(removal));
        QTRY_VERIFY(!encryptedUiItem(window,"keyServiceResponse-"+first));
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+second));
        QCOMPARE(displayed(c.f.controller,second).value("id").toString(),second); // Surviving response identity/DTO unchanged.
        auto *nextHeader=encryptedUiItem(window,"serviceGroupToggle-"+second); QVERIFY(nextHeader);
        keyMissingRequestDisclosure(window,nextHeader,Qt::Key_Space); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(!encryptedUiItem(window,"keyServiceResponse-"+second));
        auto *loader=encryptedUiItem(window,"serviceBodyLoader-"+second); QVERIFY(loader); QVERIFY(!loader->property("item").value<QObject *>());
        QTRY_VERIFY(!c.pending()); QVERIFY(c.flush()); const auto body=c.diskBody(); QVERIFY(body); QCOMPARE(body->row,c.request);
        QCOMPARE(c.lookupCount,1); // Collapse does not abort legitimate shared work or turn it into another GET.
        keyMissingRequestDisclosure(window,nextHeader,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponseText-"+second));
        QTRY_COMPARE(missingRequestUiTitle(window,second),missingRequestUiExpectedTitle(c,false,false,"en"));
        QVERIFY(c.metadata(second).value("recipientKnown").toBool()); QCOMPARE(c.lookupCount,1);
        QCOMPARE(c.f.controller.messages().size(),1); QVERIFY(displayed(c.f.controller,c.requestId).isEmpty()); QCOMPARE(c.f.posts,0);
    }

    void uiMissingRequestWorkerFallbackRemainsVisibleAfterFailedRefresh_data()
    {
        QTest::addColumn<bool>("history"); QTest::newRow("exact-body")<<false; QTest::newRow("indexed-history")<<true;
    }
    void uiMissingRequestWorkerFallbackRemainsVisibleAfterFailedRefresh()
    {
        QFETCH(bool,history); MixedOutcomeCase source; QVERIFY(source.start()); MissingRequestCase c; QVERIFY(c.hasApi());
        QVERIFY(prepareExternalMissingRequestUi(c,source));
        if(history) QVERIFY(c.putHistory({NativeCache::Kind::History,streamId,secondTopicId,"saved-R"},c.request));
        else QVERIFY(c.putBody(c.request));
        QVERIFY(c.flush()); c.lookupStatus=503; c.lookupRows=QJsonObject{};
        const auto id=c.f.cipher.value("uuid").toString();
        NativePreferences preferences(c.f.directory.filePath("missing-request-cache.ini")); preferences.setOwner(c.f.controller.ownerKey());
        QQmlApplicationEngine engine; loadNativeUi(engine,&c.f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Cached missing-R UI did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id)); auto *header=encryptedUiItem(window,"serviceGroupToggle-"+id); QCOMPARE(c.lookupCount,0);
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed()); QTRY_COMPARE(c.lookupCount,1); QTRY_VERIFY(c.pending());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponseText-"+id));
        QTRY_COMPARE(missingRequestUiTitle(window,id),missingRequestUiExpectedTitle(c,false,false,"en"));
        QVERIFY(c.pending()); // Worker body genuinely published before the held refresh completes.
        QTRY_VERIFY(!c.pending()); QVERIFY(c.flush()); const auto saved=c.diskBody(); QVERIFY(saved); QCOMPARE(saved->row,c.request);
        keyMissingRequestDisclosure(window,header,Qt::Key_Space); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(!encryptedUiItem(window,"keyServiceResponse-"+id));
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+id)); QCOMPARE(c.lookupCount,1); QCOMPARE(c.f.posts,0);
    }

    void uiMissingRequestGenuineRuntimeResetReassertsAnUnchangedMountedBody()
    {
        MixedOutcomeCase source; QVERIFY(source.start()); MissingRequestCase c; QVERIFY(c.hasApi());
        QVERIFY(prepareExternalMissingRequestUi(c,source)); const auto id=c.f.cipher.value("uuid").toString();
        NativePreferences preferences(c.f.directory.filePath("missing-request-reset.ini")); preferences.setOwner(c.f.controller.ownerKey());
        QQmlApplicationEngine engine; loadNativeUi(engine,&c.f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Reset missing-R UI did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id)); auto *header=encryptedUiItem(window,"serviceGroupToggle-"+id);
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed()); QTRY_COMPARE(c.lookupCount,1); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount,1);
        QPointer<QQuickItem> body=encryptedUiItem(window,"keyServiceResponse-"+id); QVERIFY(body);
        const auto owner=c.f.controller.ownerKey(); QVERIFY(c.f.controller.encryption().value("available").toBool());
        // This public call genuinely resets the Controller runtime/epoch while
        // available/owner/route/response UUID stay equal. It is not a fake state signal.
        c.f.controller.useChatKeys(&c.f.vault);
        QCOMPARE(c.f.controller.ownerKey(),owner); QVERIFY(c.f.controller.encryption().value("available").toBool());
        QTRY_COMPARE(c.lookupCount,2); QTRY_VERIFY(c.pending());
        QTRY_VERIFY(body && encryptedUiItem(window,"keyServiceResponse-"+id)==body.data());
        QTRY_COMPARE(missingRequestUiTitle(window,id),missingRequestUiExpectedTitle(c,false,false,"en"));
        QTRY_VERIFY(!c.pending()); QVERIFY(c.flush()); QVERIFY(c.diskBody()); QCOMPARE(c.lookupCount,2); QCOMPARE(c.f.posts,0);
    }

    void uiMissingRequestOwnerReplacementCannotPopulateTheNewOwnersBody()
    {
        MixedOutcomeCase source; QVERIFY(source.start()); MissingRequestCase c; QVERIFY(c.hasApi());
        QVERIFY(prepareExternalMissingRequestUi(c,source)); Fixture nextOwner;
        const auto id=c.f.cipher.value("uuid").toString();
        NativePreferences preferences(c.f.directory.filePath("missing-request-owner.ini")); preferences.setOwner(c.f.controller.ownerKey());
        QQmlApplicationEngine engine; loadNativeUi(engine,&c.f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Owner missing-R UI did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id)); auto *header=encryptedUiItem(window,"serviceGroupToggle-"+id);
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed()); QTRY_COMPARE(c.lookupCount,1); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount,1);
        const auto oldOwner=c.f.controller.ownerKey(); QVERIFY(!oldOwner.isEmpty());
        c.f.controller.login(nextOwner.tls.origin().toString(),"Synthetic next owner","synthetic-password","");
        QTRY_COMPARE(c.f.controller.projects().size(),1); c.f.controller.openProject(projectId);
        QTRY_VERIFY(c.f.controller.authenticated() && !c.f.controller.ownerKey().isEmpty() && c.f.controller.ownerKey()!=oldOwner);
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(!encryptedUiItem(window,"keyServiceResponse-"+id));
        QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(c.flush()); QVERIFY(!c.diskBody()); QCOMPARE(c.lookupCount,1);
        QVERIFY(!displayed(c.f.controller,id).value("keyServiceResponse").toMap().value("recipientKnown").toBool());
        QCOMPARE(c.f.posts,0); QCOMPARE(nextOwner.posts,0);
    }

    void uiMissingRequestOnlyANewSemanticDisclosureCanRetryFailure_data()
    {
        QTest::addColumn<QString>("result"); QTest::newRow("transport503-reopens")<<QString("transport");
        QTest::newRow("200-omission-reopens")<<QString("omission"); QTest::newRow("typed-negative-sealed")<<QString("negative");
    }
    void uiMissingRequestOnlyANewSemanticDisclosureCanRetryFailure()
    {
        QFETCH(QString,result); MixedOutcomeCase source; QVERIFY(source.start()); MissingRequestCase c; QVERIFY(c.hasApi());
        QVERIFY(prepareExternalMissingRequestUi(c,source));
        if(result=="transport") {c.lookupStatus=503; c.lookupRows=QJsonObject{};}
        else if(result=="omission") c.lookupRows=QJsonArray{};
        else {auto negative=c.request; negative["payload"]=QJsonObject{{"kind","markdown"},{"content","Canonical negative request body"}};c.lookupRows=QJsonArray{negative};}
        const auto id=c.f.cipher.value("uuid").toString();
        NativePreferences preferences(c.f.directory.filePath("missing-request-reopen.ini")); preferences.setOwner(c.f.controller.ownerKey());
        QQmlApplicationEngine engine; loadNativeUi(engine,&c.f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Reopen missing-R UI did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id)); auto *header=encryptedUiItem(window,"serviceGroupToggle-"+id);
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed()); QTRY_COMPARE(c.lookupCount,1); QTRY_VERIFY(c.pending());
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.lookupCount,1); QVERIFY(!c.metadata(id).value("recipientKnown").toBool());
        c.f.controller.setDraft("Routine same-context state publication"); // Real existing stateChanged, no lifetime reset.
        auto *list=encryptedUiItem(window,"messageList"); QVERIFY(list);
        QVERIFY(QMetaObject::invokeMethod(list,"rebuildServiceView")); // Controlled existing QML reproject, not disclosure.
        QSignalSpy frame(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!frame.isEmpty());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+id)); QCOMPARE(c.lookupCount,1);
        keyMissingRequestDisclosure(window,header,Qt::Key_Space); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(!encryptedUiItem(window,"keyServiceResponse-"+id));
        c.lookupStatus=200; c.lookupRows=QJsonArray{c.request};
        keyMissingRequestDisclosure(window,header,Qt::Key_Return); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+id));
        if(result=="transport" || result=="omission") {
            QTRY_COMPARE(c.lookupCount,2); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount,2);
            QTRY_COMPARE(missingRequestUiTitle(window,id),missingRequestUiExpectedTitle(c,false,false,"en"));
            QTRY_VERIFY(!c.pending());
        } else {
            QSignalSpy reopened(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!reopened.isEmpty());
            QCOMPARE(c.lookupCount,1); QVERIFY(!c.metadata(id).value("recipientKnown").toBool());
        }
        QCOMPARE(c.f.posts,0); QVERIFY(displayed(c.f.controller,c.requestId).isEmpty());
    }

    // Controlled reducer entry only. Native has no wired user selection flow;
    // this does not claim physical checkbox/selection parity.
    void uiMissingRequestSelectionReducerRemountGetsANewDisclosure()
    {
        MixedOutcomeCase source; QVERIFY(source.start()); MissingRequestCase c; QVERIFY(c.hasApi());
        QVERIFY(prepareExternalMissingRequestUi(c,source)); c.lookupStatus=503; c.lookupRows=QJsonObject{};
        const auto id=c.f.cipher.value("uuid").toString();
        NativePreferences preferences(c.f.directory.filePath("missing-request-selection.ini")); preferences.setOwner(c.f.controller.ownerKey());
        QQmlApplicationEngine engine; loadNativeUi(engine,&c.f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Selection reducer missing-R UI did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id));
        auto *header=encryptedUiItem(window,"serviceGroupToggle-"+id), *list=encryptedUiItem(window,"messageList"); QVERIFY(list);
        QVERIFY(!header->property("expanded").toBool()); QCOMPARE(c.lookupCount,0);
        QVERIFY(list->setProperty("serviceSelectionMode",true));
        QTRY_VERIFY(header->property("expanded").toBool()); QVERIFY(!header->isEnabled());
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+id)); QTRY_COMPARE(c.lookupCount,1); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount,1);
        QTRY_VERIFY(!c.pending()); QTRY_VERIFY(c.f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(serviceUiProperty(list,"serviceSession").toMap().value("preferences").toMap().isEmpty());
        QVERIFY(list->setProperty("serviceSelectionMode",false));
        QTRY_VERIFY(!header->property("expanded").toBool()); QTRY_VERIFY(!encryptedUiItem(window,"keyServiceResponse-"+id));
        QVERIFY(serviceUiProperty(list,"serviceSession").toMap().value("preferences").toMap().isEmpty());
        c.lookupStatus=200; c.lookupRows=QJsonArray{c.request};
        QVERIFY(list->setProperty("serviceSelectionMode",true));
        QTRY_VERIFY(encryptedUiItem(window,"keyServiceResponse-"+id)); QTRY_COMPARE(c.lookupCount,2); QTRY_VERIFY(c.pending()); QCOMPARE(c.lookupCount,2);
        QTRY_COMPARE(missingRequestUiTitle(window,id),missingRequestUiExpectedTitle(c,false,false,"en"));
        QTRY_VERIFY(!c.pending()); QCOMPARE(c.f.posts,0);
    }

    void uiCollapsedServiceRunPreservesSourceOuterGroupGap_data() {
        QTest::addColumn<QString>("locale");
        QTest::addColumn<int>("services");
        QTest::newRow("en-dark-two-terminal-events") << QString("en") << 2;
        QTest::newRow("ru-dark-five-action-required-events") << QString("ru") << 5;
    }
    void uiCollapsedServiceRunPreservesSourceOuterGroupGap() {
        QFETCH(QString, locale); QFETCH(int, services);
        MixedOutcomeCase source; QVERIFY(source.start());
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        const QString beforeId="10000000-0000-4000-8000-000000000001", afterId="10000000-0000-4000-8000-000000000002";
        const QList<QJsonObject> controls{source.peer.requestDto,source.reject,source.grant,
            groupingAnnouncement("10000000-0000-4000-8000-000000000003",4),
            groupingAnnouncement("10000000-0000-4000-8000-000000000004",5)};
        QJsonArray history{groupingMessage("Before service group",beforeId,0)};
        QString headId;
        for(int index=0;index<services;++index) {
            auto row=controls.at(index); row["topic_uuid"]=topicId;
            row["created_at"]=QString("2026-01-02T10:%1:00.000Z").arg(index+1,2,10,QChar('0'));
            QVERIFY(parseMessage(row,projectId));
            if(index==0) headId=row.value("uuid").toString();
            history.append(row);
        }
        history.append(groupingMessage("After service group",afterId,6));
        useGroupingHistory(f,history); QTRY_COMPARE(f.controller.messages().size(),history.size());
        QTRY_VERIFY(f.controller.property("messageHistoryReady").toBool());
        // History admission precedes the bounded control/authority work. The
        // first matching canonical J makes this external R terminal in both cases.
        QTRY_COMPARE(incomingKeyRequestRow(f.controller,headId).value("status").toString(),QString("rejected"));
        const auto settledRequest=incomingKeyRequestRow(f.controller,headId);
        QCOMPARE(settledRequest.value("id").toString(),headId);
        QCOMPARE(settledRequest.value("authorId").toString(),SourceRecipient);
        QCOMPARE(settledRequest.value("keyUuid").toString(),returnedId);
        QVERIFY(!settledRequest.value("ownCurrentDevice").toBool());
        QVERIFY(f.streamGets>0 && f.bindingGets>0); // Actual authority reads occurred.
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        NativePreferences preferences(f.directory.filePath("service-outer-gap.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode("dark");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Service gap window did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        auto *list=encryptedUiItem(window,"messageList"); QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(),3);
        QTRY_VERIFY(!list->property("positioning").toBool());
        QSignalSpy rendered(window,&QQuickWindow::frameSwapped); QVERIFY(rendered.isValid()); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *header=encryptedUiItem(window,"serviceGroupToggle-"+headId);
        auto *before=encryptedUiItem(window,"message-"+beforeId), *after=encryptedUiItem(window,"message-"+afterId);
        QVERIFY(header && before && after); QVERIFY(!header->property("expanded").toBool());
        QTRY_COMPARE(header->property("status").toString(),services==5?QString("action-required"):QString{});
        QCOMPARE(header->height(),qreal(28)); QCOMPARE(before->height(),qreal(58)); QCOMPARE(after->height(),qreal(58));
        auto *loader=encryptedUiItem(window,"serviceBodyLoader-"+headId); QVERIFY(loader);
        QVERIFY(!loader->property("active").toBool()); QVERIFY(!loader->property("item").value<QObject *>());
        const QRectF headerRect(header->mapToScene(QPointF{}),header->size());
        const QRectF beforeRect(before->mapToScene(QPointF{}),before->size()), afterRect(after->mapToScene(QPointF{}),after->size());
        const QRectF viewport(list->mapToScene(QPointF{}),list->size());
        QVERIFY(viewport.contains(beforeRect) && viewport.contains(headerRect) && viewport.contains(afterRect));
        // Actual Source31 EN-dark/RU-dark one-topic aggregate measurements:
        // before.bottom247, toggle263..291, after.top307. Test relative gaps,
        // independent of absolute shell placement and fractional text widths.
        QCOMPARE(headerRect.top()-beforeRect.bottom(),qreal(16));
        QCOMPARE(afterRect.top()-headerRect.bottom(),qreal(16));
        QCOMPARE(list->property("count").toInt(),3); QCOMPARE(f.controller.messages().size(),history.size());
        // A terminal run has no following author-group gap. Keep its existing
        // outer4/inner4 padding after a real deletion of the following message.
        auto deletion=cryptoEvent(history.last().toObject());
        auto payload=deletion.value("payload").toObject(); payload["kind"]="message.deleted"; deletion["payload"]=payload;
        QVERIFY(f.controller.applyRealtimeEvent(deletion));
        QTRY_COMPARE(f.controller.messages().size(),history.size()-1); QTRY_COMPARE(list->property("count").toInt(),2);
        QSignalSpy tailFrame(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!tailFrame.isEmpty());
        auto *tail=encryptedUiItem(window,"serviceViewRow-"+headId);
        auto *tailHeader=encryptedUiItem(window,"serviceGroupToggle-"+headId), *tailLoader=encryptedUiItem(window,"serviceBodyLoader-"+headId);
        QVERIFY(tail && tailHeader && tailLoader);
        QCOMPARE(tail->height(),qreal(44)); // Measured Source inner36 + pinned outer py-1(4+4).
        QVERIFY(!tailHeader->property("expanded").toBool());
        QVERIFY(!tailLoader->property("active").toBool()); QVERIFY(!tailLoader->property("item").value<QObject *>());
        QCOMPARE(f.posts,0);
    }

    void uiServiceInitialHistoryDisclosureUsesRealKeyboardAndUnmountsBodies_data() {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode"); QTest::addColumn<int>("services");
        QTest::newRow("en-light-one") << QString("en") << QString("light") << 1;
        QTest::newRow("en-dark-two") << QString("en") << QString("dark") << 2;
        QTest::newRow("ru-light-five") << QString("ru") << QString("light") << 5;
        QTest::newRow("ru-dark-five") << QString("ru") << QString("dark") << 5;
    }
    void uiServiceInitialHistoryDisclosureUsesRealKeyboardAndUnmountsBodies() {
        QFETCH(QString, locale); QFETCH(QString, mode); QFETCH(int, services);
        MixedOutcomeCase source; QVERIFY(source.start()); // Real Source R/G/J public envelopes.
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        QVERIFY(f.controller.property("historyCommitRevision").isValid());
        const auto before = f.controller.property("historyCommitRevision").toULongLong();
        const QString beforeId = "10000000-0000-4000-8000-000000000001", afterId = "10000000-0000-4000-8000-000000000002";
        auto request = source.peer.requestDto, grant = source.grant, reject = source.reject;
        QList<QJsonObject> controls{request, reject, grant,
            groupingAnnouncement("10000000-0000-4000-8000-000000000003", 4),
            groupingAnnouncement("10000000-0000-4000-8000-000000000004", 5)};
        QJsonArray history{groupingMessage("Before service group", beforeId, 0)};
        QStringList ids;
        for (int index = 0; index < services; ++index) {
            auto control = controls.at(index); control["topic_uuid"] = topicId;
            control["created_at"] = QString("2026-01-02T10:%1:00.000Z").arg(index + 1, 2, 10, QChar('0'));
            QVERIFY(parseMessage(control, projectId)); ids.append(control.value("uuid").toString()); history.append(control);
        }
        history.append(groupingMessage("After service group", afterId, 6));
        useGroupingHistory(f, history);
        QTRY_COMPARE(f.controller.messages().size(), history.size());
        QTRY_VERIFY(f.controller.property("messageHistoryReady").toBool());
        QVERIFY(f.controller.property("historyCommitRevision").toULongLong() > before);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        NativePreferences preferences(f.directory.filePath("service-initial.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([window=QPointer<QQuickWindow>(window)] { if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Service history window did not close", __FILE__, __LINE__); });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *list = encryptedUiItem(window, "messageList"); QVERIFY(list);
        auto *header = encryptedUiItem(window, "serviceGroupToggle-" + ids.first()); QVERIFY(header);
        QVERIFY(!header->property("expanded").toBool()); QCOMPARE(list->property("count").toInt(), 3);
        QCOMPARE(header->height(), qreal(28)); QCOMPARE(header->property("horizontalPadding").toReal(), qreal(4));
        auto *background=header->property("background").value<QQuickItem *>(); QVERIFY(background); QCOMPARE(background->property("radius").toReal(),qreal(4));
        auto *chevron = encryptedUiItem(window, "serviceGroupChevron-" + ids.first()); QVERIFY(chevron);
        QCOMPARE(chevron->size(), QSizeF(12, 12)); QCOMPARE(chevron->rotation(), qreal(-90));
        auto *caption = encryptedUiItem(window, "serviceGroupCaption-" + ids.first()); QVERIFY(caption);
        QCOMPARE(caption->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(caption->property("font").value<QFont>().pixelSize(),12); QCOMPARE(caption->property("lineHeight").toReal(),qreal(16));
        const auto colors=serviceUiProperty(window,"colors").toMap();
        QCOMPARE(caption->property("color").value<QColor>(),QColor(colors.value(header->property("status").toString()=="action-required"?"notice-base":"text-muted").toString()));
        const auto countText = locale == "ru" ? QString::number(services) + (services == 1 ? " событие" : services == 2 ? " события" : " событий")
            : QString::number(services) + (services == 1 ? " event" : " events");
        QCOMPARE(caption->property("text").toString(), countText);
        for (const auto &id : ids) QVERIFY(!encryptedUiItem(window, "message-" + id));
        auto *loader = encryptedUiItem(window, "serviceBodyLoader-" + ids.first()); QVERIFY(loader);
        QVERIFY(!loader->property("active").toBool()); QVERIFY(!loader->property("item").value<QObject *>());
        const auto scenario = "service-group-" + locale + '-' + mode + '-' + QString::number(services);
        captureServiceGroupingUi(window, scenario, "collapsed", ids.first(), history.size());
        QVERIFY(!QTest::currentTestFailed());
        const auto posts = f.posts;
        const auto historyGets = exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/");
        header->forceActiveFocus(Qt::TabFocusReason); QTRY_VERIFY(header->hasActiveFocus());
        QSignalSpy clicked(header, SIGNAL(clicked())); QVERIFY(clicked.isValid());
        QTest::keyClick(window, Qt::Key_Return); QTRY_COMPARE(clicked.size(), 1);
        QTRY_VERIFY(header->property("expanded").toBool());
        QTRY_COMPARE(list->property("count").toInt(), history.size());
        for (const auto &id : ids) QTRY_VERIFY(encryptedUiItem(window, "message-" + id));
        captureServiceGroupingUi(window, scenario, "expanded", ids.first(), history.size());
        QVERIFY(!QTest::currentTestFailed());
        QTest::keyClick(window, Qt::Key_Space); QTRY_COMPARE(clicked.size(), 2);
        QTRY_VERIFY(!header->property("expanded").toBool()); QTRY_COMPARE(list->property("count").toInt(), 3);
        for (const auto &id : ids) QVERIFY(!encryptedUiItem(window, "message-" + id));
        QCOMPARE(f.posts, posts); QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/"), historyGets);
        QCOMPARE(f.controller.messages().size(), history.size());
    }

    void uiServiceHistoryCommitAndRealtimeHaveDifferentExpansion_data() {
        QTest::addColumn<bool>("initializedEmpty");
        QTest::newRow("accepted-newer-history-then-consecutive-live") << false;
        QTest::newRow("verified-empty-history-first-live") << true;
    }
    void uiServiceHistoryCommitAndRealtimeHaveDifferentExpansion() {
        QFETCH(bool,initializedEmpty);
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        if(initializedEmpty) {
            useGroupingHistory(f,QJsonArray{}); QTRY_VERIFY(f.controller.messages().isEmpty());
            QTRY_VERIFY(f.controller.property("messageHistoryReady").toBool());
        }
        NativePreferences preferences(f.directory.filePath("service-origin.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([window=QPointer<QQuickWindow>(window)] { if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Service origin window did not close", __FILE__, __LINE__); });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *list = encryptedUiItem(window, "messageList"); QVERIFY(list);
        QTRY_VERIFY(f.controller.property("messageHistoryReady").toBool());
        QTRY_VERIFY(serviceUiProperty(list, "serviceSession").toMap().value("ready").toBool());
        const auto revision = f.controller.property("historyCommitRevision").toULongLong();
        if(initializedEmpty) {
            const QString id="20000000-0000-4000-8000-000000000008";
            QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(groupingAnnouncement(id,1))));
            QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id));
            QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id)->property("expanded").toBool());
            QCOMPARE(f.controller.property("historyCommitRevision").toULongLong(),revision); QCOMPARE(f.posts,0);
            return;
        }
        const QString historical = "20000000-0000-4000-8000-000000000001", continuation = "20000000-0000-4000-8000-000000000002";
        const QString separator = "20000000-0000-4000-8000-000000000003", live = "20000000-0000-4000-8000-000000000004";
        // Actual accepted later history, not synthetic messagesChanged or !busy.
        useGroupingHistory(f, QJsonArray{groupingMessage("History", messageId, 0), groupingAnnouncement(historical, 1)});
        QTRY_VERIFY(f.controller.property("historyCommitRevision").toULongLong() > revision);
        QTRY_VERIFY(encryptedUiItem(window, "serviceGroupToggle-" + historical));
        QVERIFY(!encryptedUiItem(window, "serviceGroupToggle-" + historical)->property("expanded").toBool());
        const auto committed = f.controller.property("historyCommitRevision").toULongLong();
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(groupingAnnouncement(continuation, 2))));
        QTRY_COMPARE(f.controller.messages().size(), 3);
        QCOMPARE(f.controller.property("historyCommitRevision").toULongLong(), committed);
        QVERIFY(!encryptedUiItem(window, "serviceGroupToggle-" + historical)->property("expanded").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(groupingMessage("Live separator", separator, 3))));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(groupingAnnouncement(live, 4))));
        // No event pump between separate live runs: ordinary queued positioning
        // cannot suppress the next all-new strictly-newer run.
        const QString secondLive = "20000000-0000-4000-8000-000000000006";
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(groupingMessage("Second separator", "20000000-0000-4000-8000-000000000007", 5))));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(groupingAnnouncement(secondLive, 6))));
        QTRY_VERIFY(encryptedUiItem(window, "serviceGroupToggle-" + live));
        QTRY_VERIFY(encryptedUiItem(window, "serviceGroupToggle-" + live)->property("expanded").toBool());
        QTRY_VERIFY(encryptedUiItem(window, "serviceGroupToggle-" + secondLive));
        QTRY_VERIFY(encryptedUiItem(window, "serviceGroupToggle-" + secondLive)->property("expanded").toBool());
        QVERIFY(encryptedUiItem(window, "keyAnnouncementService-" + live));
        QCOMPARE(f.controller.property("historyCommitRevision").toULongLong(), committed);
        const auto posts = f.posts, gets = exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/");
        auto *toggle = encryptedUiItem(window, "serviceGroupToggle-" + live);
        QSignalSpy clicked(toggle, SIGNAL(clicked())); QVERIFY(clicked.isValid()); encryptedUiClick(toggle); QTRY_COMPARE(clicked.size(), 1);
        QTRY_VERIFY(!toggle->property("expanded").toBool());
        auto next = groupingAnnouncement("20000000-0000-4000-8000-000000000005", 4);
        next["created_at"] = "2026-01-02T10:04:30.000Z";
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(next))); QTRY_COMPARE(f.controller.messages().size(), 8);
        QVERIFY(!toggle->property("expanded").toBool()); // Explicit choice beats continuation.
        QCOMPARE(f.posts, posts); QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/"), gets);
    }

    void uiServiceLargeCollapsedHistoryHasOneDelegateAndNoMissingRFetch() {
        MixedOutcomeCase source; QVERIFY(source.start());
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        QJsonArray history{groupingMessage("Before virtual history", messageId, 0)};
        auto response = source.reject;
        response["created_at"] = "2026-01-02T10:01:00.000Z";
        response["topic_uuid"] = topicId; history.append(response); // R deliberately outside this history.
        for (int index = 0; index < 498; ++index) {
            const auto id = QUuid::createUuidV5(QUuid(projectId), QString("cassi-service-%1").arg(index).toUtf8()).toString(QUuid::WithoutBraces);
            auto row = groupingAnnouncement(id, 2); row["created_at"] = QString("2026-01-02T10:02:%1.%2Z").arg(index/100,2,10,QChar('0')).arg(index%100,3,10,QChar('0'));
            QVERIFY(parseMessage(row, projectId)); history.append(row);
        }
        useGroupingHistory(f, history); QTRY_COMPARE(f.controller.messages().size(), 500);
        QTRY_VERIFY(f.controller.property("messageHistoryReady").toBool()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        NativePreferences preferences(f.directory.filePath("service-virtual.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([window=QPointer<QQuickWindow>(window)] { if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Service virtual window did not close", __FILE__, __LINE__); });
        QVERIFY(QTest::qWaitForWindowExposed(window)); QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *list = encryptedUiItem(window, "messageList"); QVERIFY(list);
        QCOMPARE(list->property("count").toInt(), 2); // Not499zero-height rows with spacing.
        const auto id = response.value("uuid").toString();
        auto *header = encryptedUiItem(window, "serviceGroupToggle-" + id); QVERIFY(header); QVERIFY(!header->property("expanded").toBool());
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + source.peer.requestId), 0);
        QVERIFY(!encryptedUiItem(window, "keyServiceResponse-" + id));
        const auto children = serviceUiEvaluate(list, "(function(){let count=0;for(const child of contentItem.children)if(child.objectName.indexOf('serviceViewRow-')===0)++count;return count;})()").toInt();
        QVERIFY(children > 0 && children <= 2);
        QCOMPARE(serviceUiProperty(list, "serviceAliases").toMap().size(), 499);
        // Expanded missing-R resolution is a separate Core feature, NOT claimed here.
        QCOMPARE(f.posts, 0);
    }

    void uiServiceProductionReducerPreservesManualMergeFocusAndContext() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        NativePreferences preferences(f.directory.filePath("service-reducer.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([window=QPointer<QQuickWindow>(window)] { if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Service reducer window did not close", __FILE__, __LINE__); });
        QVERIFY(QTest::qWaitForWindowExposed(window)); auto *list = encryptedUiItem(window, "messageList"); QVERIFY(list);
        // Controlled state-reducer checks against the actual production function,
        // not a copied implementation or a claim of Native selection/navigation UI.
        const auto result = serviceUiEvaluate(list, R"JS((function() {
            const a={ids:['a','b']}, b={ids:['c']}, merged={ids:['a','b','c']};
            let s={preferences:{},revision:0};
            s=withServicePreference(s,[a],'auto',true);
            s=withServicePreference(s,[a],'manual',false);
            s=withServicePreference(s,[b],'manual',true);
            const winner=servicePreference(merged,s);
            return {manual:winner.source==='manual', latest:winner.expanded===true,
                splitA:servicePreference(a,s).expanded===false,
                splitB:servicePreference(b,s).expanded===true,
                priority:serviceGroupStatus([{serviceStatus:'unavailable'},{serviceStatus:'waiting'},{serviceStatus:'action-required'}])};
        })())JS").toMap();
        QVERIFY(!QTest::currentTestFailed()); QVERIFY(result.value("manual").toBool()); QVERIFY(result.value("latest").toBool());
        QVERIFY(result.value("splitA").toBool()); QVERIFY(result.value("splitB").toBool()); QCOMPARE(result.value("priority").toString(), QString("action-required"));
        const auto posts = f.posts;
        const QString id = "30000000-0000-4000-8000-000000000001";
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(groupingAnnouncement(id, 1))));
        QTRY_VERIFY(encryptedUiItem(window, "serviceGroupToggle-" + id));
        auto *toggle = encryptedUiItem(window, "serviceGroupToggle-" + id);
        if (toggle->property("expanded").toBool()) { QSignalSpy clicked(toggle,SIGNAL(clicked())); encryptedUiClick(toggle); QTRY_COMPARE(clicked.size(),1); }
        QTRY_VERIFY(!toggle->property("expanded").toBool());
        QVERIFY(QQmlProperty(list,"serviceSelectionMode").write(true)); QTRY_VERIFY(toggle->property("expanded").toBool()); QVERIFY(!toggle->isEnabled());
        QVERIFY(QQmlProperty(list,"serviceSelectionMode").write(false)); QTRY_VERIFY(!toggle->property("expanded").toBool());
        const QVariantMap target{{"intentId","controlled-reducer"},{"messageUuid",id},{"focusAttempt",1}};
        QVERIFY(QQmlProperty(list,"serviceFocusedTarget").write(target)); QTRY_VERIFY(toggle->property("expanded").toBool());
        { QSignalSpy clicked(toggle,SIGNAL(clicked())); encryptedUiClick(toggle); QTRY_COMPARE(clicked.size(),1); }
        QVERIFY(QQmlProperty(list,"serviceFocusedTarget").write(QVariantMap{})); QVERIFY(QQmlProperty(list,"serviceFocusedTarget").write(target));
        QVERIFY(!toggle->property("expanded").toBool()); // Same triple cannot undo explicit collapse.
        auto retry = target; retry["focusAttempt"] = 2;
        QVERIFY(QQmlProperty(list,"serviceFocusedTarget").write(retry)); QTRY_VERIFY(toggle->property("expanded").toBool());
        const auto scope = serviceUiProperty(list,"serviceSession").toMap().value("scope").toString();
        // The default fixture returns the first topic's cipher for every history
        // query. Admit an actual empty history for this different route instead.
        const auto secondTopicHistoryGets = std::make_shared<int>(0);
        const auto previous = f.http.handler;
        f.http.handler = [previous, secondTopicHistoryGets](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/"
                && QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId) {
                ++*secondTopicHistoryGets;
                return {QJsonArray{}};
            }
            return previous(request);
        };
        f.controller.selectTopic(secondTopicId); QTRY_VERIFY(f.controller.property("messageHistoryReady").toBool());
        QCOMPARE(*secondTopicHistoryGets, 1);
        QCOMPARE(f.controller.selectedTopic(), secondTopicId);
        QVERIFY(f.controller.messages().isEmpty());
        QTRY_VERIFY(serviceUiProperty(list,"serviceSession").toMap().value("scope").toString()!=scope);
        QCOMPARE(f.posts, posts);
    }

    void uiServiceManualChoiceFollowsActualSplitMergeAndHeadDeletion() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        const QString a="40000000-0000-4000-8000-000000000001", b="40000000-0000-4000-8000-000000000002", separator="40000000-0000-4000-8000-000000000003";
        auto first=groupingAnnouncement(a,1), second=groupingAnnouncement(b,3), middle=groupingMessage("Separator",separator,2);
        useGroupingHistory(f,QJsonArray{first,middle,second}); QTRY_COMPARE(f.controller.messages().size(),3);
        NativePreferences preferences(f.directory.filePath("service-split.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Service split window did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *list=encryptedUiItem(window,"messageList"); QVERIFY(list);
        const auto click=[&](const QString &id) {
            auto *button=encryptedUiItem(window,"serviceGroupToggle-"+id); QVERIFY(button);
            QSignalSpy clicked(button,SIGNAL(clicked())); QVERIFY(clicked.isValid()); encryptedUiClick(button); QTRY_COMPARE(clicked.size(),1);
        };
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+a));
        click(a); QVERIFY(!QTest::currentTestFailed()); // older manual open
        click(b); QVERIFY(!QTest::currentTestFailed());
        click(b); QVERIFY(!QTest::currentTestFailed()); // newer manual close
        auto deletion=cryptoEvent(middle); auto payload=deletion.value("payload").toObject(); payload["kind"]="message.deleted"; deletion["payload"]=payload;
        QVERIFY(f.controller.applyRealtimeEvent(deletion)); QTRY_COMPARE(f.controller.messages().size(),2);
        QTRY_COMPARE(list->property("count").toInt(),1);
        // count() observes the model before ItemView's queued layout/removal.
        // Record this boundary, then keep the original rendered-item oracle.
        const auto mergedState = [&] {
            auto *header = encryptedUiItem(window,"serviceGroupToggle-"+b);
            return QJsonObject{{"count",list->property("count").toInt()},
                {"positioning",list->property("positioning").toBool()},
                {"oldHeaderFound",bool(header)}, {"sameWindow",header && header->window()==window},
                {"oldHeaderRect",header ? encryptedUiRect(header,window->contentItem()) : QJsonObject{}},
                {"windowActive",window->isActive()}};
        };
        const auto beforeFrame = mergedState();
        QJsonObject afterFrame;
        QSignalSpy mergedFrame(window,&QQuickWindow::frameSwapped); QVERIFY(mergedFrame.isValid());
        const auto traceMerge = qScopeGuard([&] {
            qInfo().noquote() << "SERVICE_MERGE_FRAME" << QJsonDocument(QJsonObject{
                {"before",beforeFrame},{"after",afterFrame},{"frames",mergedFrame.size()}}).toJson(QJsonDocument::Compact);
        });
        window->update(); QTRY_VERIFY(!mergedFrame.isEmpty());
        afterFrame = mergedState();
        QVERIFY(!encryptedUiItem(window,"serviceGroupToggle-"+a)->property("expanded").toBool());
        QVERIFY(!encryptedUiItem(window,"serviceGroupToggle-"+b)); // merged, latest manual wins
        second["topic_uuid"]=secondTopicId;
        auto move=cryptoEvent(second); payload=move.value("payload").toObject(); payload["kind"]="message.updated"; move["payload"]=payload;
        QVERIFY(f.controller.applyRealtimeEvent(move)); QTRY_COMPARE(list->property("count").toInt(),2);
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+b));
        QVERIFY(!encryptedUiItem(window,"serviceGroupToggle-"+b)->property("expanded").toBool());
        click(b); QVERIFY(!QTest::currentTestFailed());
        const QString prepend="40000000-0000-4000-8000-000000000004";
        auto prior=groupingAnnouncement(prepend,2,secondTopicId);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(prior)));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+prepend));
        QVERIFY(encryptedUiItem(window,"serviceGroupToggle-"+prepend)->property("expanded").toBool());
        deletion=cryptoEvent(prior); payload=deletion.value("payload").toObject(); payload["kind"]="message.deleted"; deletion["payload"]=payload;
        QVERIFY(f.controller.applyRealtimeEvent(deletion));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+b));
        QVERIFY(encryptedUiItem(window,"serviceGroupToggle-"+b)->property("expanded").toBool());
        QCOMPARE(f.posts,0);
    }

    void uiServiceAutomaticOffscreenCollapsePreservesAnchorAndManualChoice() {
        MixedOutcomeCase source; QVERIFY(source.start());
        Fixture f; f.stream=fullEncryptedStream(); QVERIFY(f.start());
        NativePreferences preferences(f.directory.filePath("service-offscreen.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Service offscreen window did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); auto *list=encryptedUiItem(window,"messageList"); QVERIFY(list);
        QTRY_VERIFY(serviceUiProperty(list,"serviceSession").toMap().value("ready").toBool());
        auto grant=source.grant; grant["topic_uuid"]=topicId; grant["created_at"]="2026-01-02T10:01:00.000Z";
        const auto id=grant.value("uuid").toString();
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant)));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id));
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id)->property("expanded").toBool());
        QCOMPARE(displayed(f.controller,id).value("serviceStatus").toString(),QString{});
        // Controlled text-selection reducer barrier: there is no Native text
        // range UI yet. The real rendered/offscreen/anchor path below is tested.
        QVERIFY(QQmlProperty(list,"serviceTextSelectionIds").write(QStringList{id}));
        for(int index=0;index<80;++index) {
            const auto nextId=QUuid::createUuidV5(QUuid(projectId),QString("cassi-after-group-%1").arg(index).toUtf8()).toString(QUuid::WithoutBraces);
            auto ordinary=groupingMessage(QString("Ordinary server row %1").arg(index),nextId,2);
            ordinary["created_at"]=QString("2026-01-02T10:02:%1.000Z").arg(index,2,10,QChar('0'));
            // Keep timestamp wire-valid for all80rows, with strict order.
            ordinary["created_at"]=QDateTime::fromString("2026-01-02T10:02:00Z",Qt::ISODate).addSecs(index).toString(Qt::ISODateWithMs);
            QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(ordinary)));
        }
        QTRY_COMPARE(f.controller.messages().size(),82);
        serviceUiEvaluate(list,"positionViewAtEnd()"); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(!list->property("positioning").toBool());
        auto preferencesMap=serviceUiProperty(list,"serviceSession").toMap().value("preferences").toMap();
        QVERIFY(preferencesMap.value(id).toMap().value("expanded").toBool());
        const auto before=serviceUiEvaluate(list,"captureViewport()").toMap(); QVERIFY(!QTest::currentTestFailed());
        QVERIFY(!before.value("anchorId").toString().isEmpty());
        QVERIFY(QQmlProperty(list,"serviceTextSelectionIds").write(QStringList{}));
        QTRY_VERIFY(!serviceUiProperty(list,"serviceSession").toMap().value("preferences").toMap().value(id).toMap().value("expanded").toBool());
        QTRY_VERIFY(!list->property("positioning").toBool());
        const auto after=serviceUiEvaluate(list,"captureViewport()").toMap(); QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(after.value("anchorId").toString(),before.value("anchorId").toString());
        QVERIFY(qAbs(after.value("offset").toReal()-before.value("offset").toReal())<=1);
        QCOMPARE(f.posts,0);
        // Inspect a queued stale revision after an actual manual reveal. Its
        // captured work cannot override the newer preference; no extra click.
        const auto oldTicket=list->property("serviceInspectionTicket").toInt();
        const auto oldRevision=serviceUiProperty(list,"serviceSession").toMap().value("revision").toInt();
        const auto scope=serviceUiProperty(list,"serviceSession").toMap().value("scope").toString();
        serviceUiEvaluate(list,"positionViewAtBeginning()"); QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY(encryptedUiItem(window,"serviceGroupToggle-"+id));
        auto *toggle=encryptedUiItem(window,"serviceGroupToggle-"+id); QSignalSpy clicked(toggle,SIGNAL(clicked())); QVERIFY(clicked.isValid());
        encryptedUiClick(toggle); QTRY_COMPARE(clicked.size(),1); QTRY_VERIFY(toggle->property("expanded").toBool());
        serviceUiEvaluate(list,QString("inspectServiceGroups(%1,%2,%3)").arg(oldTicket).arg(oldRevision).arg(QString::fromUtf8(QJsonDocument(QJsonArray{scope}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)));
        QVERIFY(!QTest::currentTestFailed()); QVERIFY(toggle->property("expanded").toBool());
    }

    void uiCollapsedServiceReadBoundaryUsesLatestActualSameTopicUuid_data() {
        QTest::addColumn<bool>("reverseMicroseconds");
        QTest::newRow("distinct-milliseconds") << false;
        QTest::newRow("same-ms-reverse-uuid-exact-micros") << true;
    }
    void uiCollapsedServiceReadBoundaryUsesLatestActualSameTopicUuid() {
        QFETCH(bool,reverseMicroseconds);
        Fixture f; f.stream=fullEncryptedStream(); QVERIFY(f.start());
        const QString first=reverseMicroseconds?"50000000-0000-4000-8000-000000000002":"50000000-0000-4000-8000-000000000001";
        const QString last=reverseMicroseconds?"50000000-0000-4000-8000-000000000001":"50000000-0000-4000-8000-000000000002";
        auto a=groupingAnnouncement(first,1), b=groupingAnnouncement(last,2);
        if(reverseMicroseconds) {
            a["created_at"]="2026-01-02T10:01:00.000001Z";
            b["created_at"]="2026-01-02T10:01:00.000002Z";
            QVERIFY(first>last); // Public visual UUID order reverses exact chronology.
        }
        a["read"]=false; b["read"]=false;
        const auto rows=std::make_shared<QJsonArray>(QJsonArray{a,b});
        QString unseen;
        for(int index=0;index<100;++index) {
            unseen=QUuid::createUuidV5(QUuid(projectId),QString("cassi-unseen-tail-%1").arg(index).toUtf8()).toString(QUuid::WithoutBraces);
            auto row=groupingMessage(QString("Other-topic unread row %1").arg(index),unseen,3,secondTopicId);
            row["created_at"]=QDateTime::fromString("2026-01-02T10:03:00Z",Qt::ISODate).addSecs(index).toString(Qt::ISODateWithMs);
            row["read"]=false; rows->append(row);
        }
        const auto anchors=std::make_shared<QStringList>(); const auto previous=f.http.handler;
        f.http.handler=[rows,anchors,previous](const Request &request)->Response {
            const auto path=request.target.path();
            if(request.method=="GET" && path=="/api/workspace/v1/messenger/messages/")return {*rows};
            if(path.endsWith("/actions/read_up_to/invoke")) {
                const auto id=path.section('/',-4,-4); int boundary=-1;
                for(int index=0;index<rows->size();++index)if(rows->at(index).toObject().value("uuid").toString()==id)boundary=index;
                if(boundary<0)return {QJsonObject{},404};
                anchors->append(id); const auto topic=rows->at(boundary).toObject().value("topic_uuid");
                for(int index=0;index<=boundary;++index) {auto row=rows->at(index).toObject();if(row.value("topic_uuid")==topic){row["read"]=true;rows->replace(index,row);}}
                return {rows->at(boundary).toObject()};
            }
            return previous(request);
        };
        f.controller.selectStream(streamId); QTRY_COMPARE(f.controller.messages().size(),102);
        const auto projected=f.controller.messages();
        QCOMPARE(projected.at(0).toMap().value("id").toString(),first);
        QCOMPARE(projected.at(1).toMap().value("id").toString(),last);
        if(reverseMicroseconds) QCOMPARE(projected.at(0).toMap().value("createdAt"),projected.at(1).toMap().value("createdAt"));
        NativePreferences preferences(f.directory.filePath("service-read.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window); window->hide();
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Service read window did not close",__FILE__,__LINE__);});
        // QML delegates have visual parents, not necessarily QObject parents.
        // Find the existing ListView while hidden, before read timers can run.
        std::function<QQuickItem *(QQuickItem *)> visualItem = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "messageList") return item;
            for (auto *child : item->childItems()) if (auto *found = visualItem(child)) return found;
            return nullptr;
        };
        auto *list=visualItem(window->contentItem()); QVERIFY(list);
        QTRY_VERIFY(!list->property("positioning").toBool());
        serviceUiEvaluate(list,"positionViewAtBeginning()"); QVERIFY(!QTest::currentTestFailed());
        window->show(); QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(!anchors->isEmpty()); QCOMPARE(anchors->first(),last);
        QTRY_VERIFY(anchors->contains(last)); QVERIFY(!anchors->contains(first));
        QVERIFY(!anchors->contains(unseen));
        QTRY_VERIFY(displayed(f.controller,first).value("read").toBool());
        QVERIFY(displayed(f.controller,last).value("read").toBool()); QVERIFY(!displayed(f.controller,unseen).value("read").toBool());
        QVERIFY(!encryptedUiItem(window,"message-"+first));
        const auto visualFirst=reverseMicroseconds?last:first;
        auto *header=encryptedUiItem(window,"serviceGroupToggle-"+visualFirst); QVERIFY(header); QVERIFY(!header->property("expanded").toBool());
        QCOMPARE(f.posts,0);
    }

    void uiServiceAccountFilterPrecedesAuthorAndTopicGrouping_data() {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("encrypted-all") << true;
        QTest::newRow("ordinary-account") << false;
    }
    void uiServiceAccountFilterPrecedesAuthorAndTopicGrouping() {
        QFETCH(bool,encrypted);
        MixedOutcomeCase source; QVERIFY(source.start());
        Fixture f; f.stream=fullEncryptedStream(); f.stream["encryption"]=encrypted;
        if(!encrypted)f.stream["current_encryption_key_uuid"]=QJsonValue::Null;
        QVERIFY(f.start());
        const QString first="60000000-0000-4000-8000-000000000001", after="60000000-0000-4000-8000-000000000002", unrelated="60000000-0000-4000-8000-000000000003", announced="60000000-0000-4000-8000-000000000004";
        auto before=groupingMessage("First ordinary",first,0), later=groupingMessage("Second ordinary",after,2);
        before["author_uuid"]=Issuer; later["author_uuid"]=Issuer; before["is_own"]=false; later["is_own"]=false;
        auto external=source.peer.requestDto; external["uuid"]=unrelated; external["topic_uuid"]=secondTopicId; external["created_at"]="2026-01-02T10:01:00.000Z";
        auto own=source.peer.requestDto; own["topic_uuid"]=topicId; own["author_uuid"]=userId; own["is_own"]=false; own["created_at"]="2026-01-02T10:03:00.000Z";
        auto grant=source.grant; grant["topic_uuid"]=topicId; grant["created_at"]="2026-01-02T10:04:00.000Z";
        auto announcement=groupingAnnouncement(announced,5); announcement["author_uuid"]=userId;
        // Source-generated public payloads with controlled canonical author DTO
        // for account projection. No ordinary-holder capability is invented.
        useGroupingHistory(f,QJsonArray{before,external,later,own,grant,announcement}); QTRY_COMPARE(f.controller.messages().size(),6);
        const auto response=displayed(f.controller,grant.value("uuid").toString()).value("keyServiceResponse").toMap();
        QVERIFY(response.value("recipientKnown").toBool()); QCOMPARE(response.value("recipientId").toString(),userId);
        QVERIFY(!response.value("ownCurrentDevice").toBool());
        NativePreferences preferences(f.directory.filePath("service-relevance.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Service account window did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *list=encryptedUiItem(window,"messageList"); QVERIFY(list);
        QCOMPARE(list->property("count").toInt(),encrypted?4:3);
        QCOMPARE(bool(encryptedUiItem(window,"serviceGroupToggle-"+unrelated)),encrypted);
        const auto ownId=own.value("uuid").toString();
        auto *caption=encryptedUiItem(window,"serviceGroupCaption-"+ownId); QVERIFY(caption);
        QCOMPARE(caption->property("text").toString(),encrypted?QString("3 events"):QString("2 events"));
        const auto aliases=serviceUiProperty(list,"serviceAliases").toMap();
        QCOMPARE(aliases.value(grant.value("uuid").toString()).toString(),ownId);
        QCOMPARE(aliases.contains(announced),encrypted); // Even own announced is hidden in account mode.
        QCOMPARE(bool(encryptedUiItem(window,"message-author-"+after)),encrypted);
        QCOMPARE(bool(encryptedUiItem(window,"message-divider-"+after)),encrypted);
        QCOMPARE(f.posts,0);
    }

    void uiServiceRunBoundariesUseActualAuthorTopicAndLocalDay() {
        MixedOutcomeCase source; QVERIFY(source.start());
        Fixture f; f.stream=fullEncryptedStream(); QVERIFY(f.start());
        auto request=source.peer.requestDto, grant=source.grant;
        request["topic_uuid"]=topicId; request["created_at"]="2026-01-02T10:01:00.000Z";
        grant["topic_uuid"]=topicId; grant["created_at"]="2026-01-02T10:02:00.000Z";
        QVERIFY(request.value("author_uuid")!=grant.value("author_uuid"));
        const QString changedTopic="80000000-0000-4000-8000-000000000001", changedDay="80000000-0000-4000-8000-000000000002", text="80000000-0000-4000-8000-000000000003", last="80000000-0000-4000-8000-000000000004";
        auto topic=groupingAnnouncement(changedTopic,3,secondTopicId), day=groupingAnnouncement(changedDay,3,secondTopicId);
        day["created_at"]="2026-01-03T10:03:00.000Z";
        auto ordinary=groupingMessage("Ordinary breaks service run",text,4,secondTopicId); ordinary["created_at"]="2026-01-03T10:04:00.000Z";
        auto after=groupingAnnouncement(last,5,secondTopicId); after["created_at"]="2026-01-03T10:05:00.000Z";
        useGroupingHistory(f,QJsonArray{request,grant,topic,day,ordinary,after}); QTRY_COMPARE(f.controller.messages().size(),6);
        NativePreferences preferences(f.directory.filePath("service-boundaries.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Service boundaries window did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); auto *list=encryptedUiItem(window,"messageList"); QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(),5);
        const auto first=request.value("uuid").toString();
        auto *caption=encryptedUiItem(window,"serviceGroupCaption-"+first); QVERIFY(caption); QCOMPARE(caption->property("text").toString(),QString("2 events"));
        QVERIFY(!encryptedUiItem(window,"serviceTopicDivider-"+first));
        QVERIFY(encryptedUiItem(window,"serviceTopicDivider-"+changedTopic));
        QVERIFY(!encryptedUiItem(window,"serviceTopicDivider-"+changedDay));
        QVERIFY(!encryptedUiItem(window,"serviceTopicDivider-"+last));
        QVERIFY(encryptedUiItem(window,"serviceGroupToggle-"+changedDay));
        QVERIFY(encryptedUiItem(window,"serviceGroupToggle-"+last));
        QVERIFY(encryptedUiItem(window,"message-"+text)); QCOMPARE(f.posts,0);
    }

    void uiServiceAnnouncementRequestsOnlyItsExactMissingGeneration() {
        Fixture f; f.stream=fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString id="70000000-0000-4000-8000-000000000001";
        useGroupingHistory(f,QJsonArray{groupingAnnouncement(id,1)}); QTRY_COMPARE(f.controller.messages().size(),1);
        QTRY_VERIFY(displayed(f.controller,id).value("keyServiceAnnouncement").toMap().value("canRequest").toBool());
        const auto metadata=displayed(f.controller,id).value("keyServiceAnnouncement").toMap();
        QCOMPARE(metadata.size(),3); QCOMPARE(metadata.value("keyUuid").toString(),returnedId); QVERIFY(metadata.value("offerRequest").toBool());
        QCOMPARE(displayed(f.controller,id).value("serviceStatus").toString(),QString("action-required"));
        NativePreferences preferences(f.directory.filePath("service-announcement.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("ru"); preferences.setMode("dark");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if(!closeEncryptedVisualWindow(window.data()))QTest::qFail("Service announcement window did not close",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *header=encryptedUiItem(window,"serviceGroupToggle-"+id); QVERIFY(header); QCOMPARE(header->property("status").toString(),QString("action-required"));
        auto *caption=encryptedUiItem(window,"serviceGroupCaption-"+id); QVERIFY(caption);
        QCOMPARE(caption->property("color").value<QColor>(),QColor(serviceUiProperty(window,"colors").toMap().value("notice-base").toString()));
        expandInitialServiceGroup(window,id); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyAnnouncementService-"+id); QVERIFY(service);
        auto *title=encryptedUiItem(window,"keyAnnouncementTitle-"+id); QVERIFY(title);
        QCOMPARE(title->property("text").toString(),QString("Ключ безопасности обновился"));
        QCOMPARE(title->property("textFormat").toInt(),int(Qt::PlainText));
        QCOMPARE(title->property("lineCount").toInt(),1); QCOMPARE(title->height(),qreal(20));
        QVERIFY(encryptedUiText(service,QString("Запросите ключ для просмотра сообщений")));
        auto *request=encryptedUiItem(window,"requestAnnouncedChatKeyButton-"+id); QVERIFY(request); QVERIFY(request->isEnabled());
        QCOMPARE(request->property("text").toString(),QString("Запросить ключ")); QCOMPARE(request->height(),qreal(20));
        const auto before=f.posts; QSignalSpy clicked(request,SIGNAL(clicked())); QVERIFY(clicked.isValid());
        encryptedUiClick(request); QTRY_COMPARE(clicked.size(),1); QTRY_COMPARE(f.posts,before+1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        const auto body=f.requestBody,payload=body.value("payload").toObject();
        QCOMPARE(payload.value("kind").toString(),QString("key_request")); QCOMPARE(payload.value("key_uuid").toString(),returnedId);
        QCOMPARE(body.value("stream_uuid").toString(),streamId); QCOMPARE(body.value("topic_uuid").toString(),secondTopicId);
        const auto canonical=QUuid::createUuidV5(QUuid(secondTopicId),body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(f.savedRequest.value("uuid").toString(),canonical); QVERIFY(!payload.contains("plaintext"));
        QTRY_VERIFY(!request->isEnabled()); QVERIFY(request->isVisible());
        QTRY_COMPARE(header->property("status").toString(),QString("waiting"));
        QTest::mouseClick(window,Qt::LeftButton,{},request->mapToScene(QPointF(request->width()/2,request->height()/2)).toPoint());
        QCOMPARE(clicked.size(),1); QCOMPARE(f.posts,before+1);
    }

    void uiEncryptedComposerSendsActualSourceCipher_data() {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("aggregate"); QTest::addColumn<bool>("historical");
        QTest::newRow("en-light-selected-current") << QString("en") << QString("light") << false << false;
        QTest::newRow("en-dark-aggregate-current") << QString("en") << QString("dark") << true << false;
        QTest::newRow("ru-light-selected-current") << QString("ru") << QString("light") << false << false;
        QTest::newRow("ru-dark-aggregate-current") << QString("ru") << QString("dark") << true << false;
        QTest::newRow("en-light-selected-historical") << QString("en") << QString("light") << false << true;
        QTest::newRow("ru-dark-aggregate-historical") << QString("ru") << QString("dark") << true << true;
    }
    void uiEncryptedComposerSendsActualSourceCipher() {
        QFETCH(QString, locale); QFETCH(QString, mode); QFETCH(bool, aggregate); QFETCH(bool, historical);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString target = aggregate ? secondTopicId : topicId;
        const QString text = "Synthetic encrypted composer input";
        SourceSendPeer peer; QVERIFY(peer.prepare(f, target, text));
        if (historical) {
            f.stream["current_encryption_key_uuid"] = OtherGeneration;
            f.stream["updated_at"] = "2026-01-02T10:00:02.000Z";
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event));
            QTRY_COMPARE(f.controller.encryption().value("currentKeyUuid").toString(), OtherGeneration);
            QTRY_VERIFY(f.controller.encryption().value("canRequestCurrentKey").toBool());
            QVERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        }
        if (aggregate) {
            f.controller.selectStream(streamId);
            QTRY_VERIFY(!f.controller.busy() && f.controller.topics().size() == 2 && f.controller.messages().size() == 1);
            QCOMPARE(f.controller.selectedTopic(), QString{});
            QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        }
        NativePreferences preferences(f.directory.filePath("composer-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Composer window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QTRY_VERIFY(encryptedUiItem(window, "messageComposer"));
        auto *composer = encryptedUiItem(window, "messageComposer"); QVERIFY(composer->isEnabled());
        QVERIFY(f.controller.property("canCompose").toBool());
        QCOMPARE(f.controller.draft(), QString{}); QCOMPARE(composer->property("text").toString(), QString{});
        auto *send = encryptedUiItem(window, "sendButton"); QVERIFY(send); QVERIFY(!send->isEnabled());
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        if (auto *attach = encryptedUiItem(window, "attachButton")) QVERIFY(!attach->isEnabled());
        if (historical) {
            QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
            QVERIFY(window->property("encryptedSendingKeyOutdated").toBool());
            QVERIFY(window->property("encryptedKeyNoticeVisible").toBool());
            auto *notice = encryptedUiItem(window, "encryptedReadStatusText"); QVERIFY(notice);
            QCOMPARE(notice->property("text").toString(), locale == "ru"
                ? QString("Ключ безопасности обновился. Запросите ключ у владельца канала для просмотра новых сообщений")
                : QString("The security key has changed. Request the key from the channel owner to read new messages"));
            QVERIFY(encryptedUiItem(window, "requestCurrentChatKeyButton"));
            QVERIFY(encryptedUiItem(window, "requestCurrentChatKeyButton")->isEnabled());
            QVERIFY(!encryptedUiItem(window, "encryptedUnsupportedSendingNotice"));
            auto *dismiss = encryptedUiItem(window, "dismissEncryptedKeyNoticeButton"); QVERIFY(dismiss);
            const auto beforeDismiss = f.posts; encryptedUiClick(dismiss);
            QTRY_VERIFY(!window->property("encryptedKeyNoticeVisible").toBool());
            QVERIFY(window->property("encryptedSendingKeyOutdated").toBool());
            QVERIFY(!encryptedUiItem(window, "requestCurrentChatKeyButton"));
            QVERIFY(composer->isEnabled()); QCOMPARE(f.posts, beforeDismiss);
            QSignalSpy dismissedFrame(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!dismissedFrame.isEmpty());
        }
        const int before = f.posts;
        encryptedUiClick(composer); QTRY_VERIFY(composer->hasActiveFocus());
        for (const auto key : text.toLatin1()) QTest::keyClick(window, key);
        QTRY_COMPARE(f.controller.draft(), text); QCOMPARE(composer->property("text").toString(), text);
        QTRY_VERIFY(f.controller.canSend()); QTRY_VERIFY(send->isEnabled());
        QSignalSpy clicked(send, SIGNAL(clicked())); QVERIFY(clicked.isValid());
        QSignalSpy sent(&f.controller, &NativeController::messageSent);
        encryptedUiClick(send); QTRY_COMPARE(clicked.size(), 1); QTRY_COMPARE(f.posts, before + 1);
        const auto body = f.requestBody, payload = body.value("payload").toObject();
        const auto localId = body.value("uuid").toString(); QVERIFY(isUuid(localId));
        QCOMPARE(body.value("stream_uuid").toString(), streamId); QCOMPARE(body.value("topic_uuid").toString(), target);
        QCOMPARE(payload.keys(), QStringList({"content", "key_uuid", "kind"}));
        QCOMPARE(payload.value("kind").toString(), QString("e2ee")); QCOMPARE(payload.value("key_uuid").toString(), returnedId);
        const auto encoded = payload.value("content").toString().toLatin1();
        const auto envelope = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        QVERIFY(envelope.size() >= 48); QVERIFY(encoded.size() <= 213336); QCOMPARE(envelope.toBase64(), encoded);
        for (const auto &posted : f.requestBodies) QVERIFY(!QJsonDocument(posted).toJson(QJsonDocument::Compact).contains(text.toUtf8()));
        const auto publicId = QUuid::createUuidV5(QUuid(target), localId.toUtf8()).toString(QUuid::WithoutBraces);
        QTRY_COMPARE(sent.size(), 1); QCOMPARE(sent.first().at(0).toString(), publicId);
        QCOMPARE(sent.first().at(1).toString(), streamId); QCOMPARE(sent.first().at(2).toString(), target);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 1);
        QCOMPARE(f.savedRequests.value(publicId).value("payload").toObject(), payload);
        const auto verified = peer.verify(payload, target);
        QVERIFY(verified.value("ok").toBool()); QVERIFY(verified.value("matchesExpected").toBool());
        QCOMPARE(verified.value("plaintextBytes").toInt(), text.toUtf8().size());
        QCOMPARE(verified.value("keyUuid").toString(), returnedId);
        QTRY_COMPARE(f.controller.draft(), QString{}); QTRY_COMPARE(composer->property("text").toString(), QString{});
        QVERIFY(composer->isEnabled()); QVERIFY(!send->isEnabled());
        QCOMPARE(f.controller.selectedStream(), streamId); QCOMPARE(f.controller.selectedTopic(), aggregate ? QString{} : topicId);
        QCOMPARE(f.posts, before + 1);
        const bool windowClosed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(windowClosed);
    }

    void uiEncryptedComposerLockedKeyNeverFallsBackToPlaintext_data() {
        QTest::addColumn<bool>("nullCurrent");
        QTest::newRow("en-light-missing-selected") << false;
        QTest::newRow("ru-dark-null-readable-aggregate") << true;
    }
    void uiEncryptedComposerLockedKeyNeverFallsBackToPlaintext() {
        QFETCH(bool, nullCurrent);
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        if (nullCurrent) {
            QVERIFY(f.prepareA()); f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
            auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
            payload["kind"] = "stream.updated"; event["payload"] = payload; QVERIFY(f.controller.applyRealtimeEvent(event));
            f.controller.selectStream(streamId);
            QTRY_VERIFY(!f.controller.busy() && f.controller.messages().size() == 1);
            QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        }
        NativePreferences preferences(f.directory.filePath("composer-locked-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(nullCurrent ? "ru" : "en"); preferences.setMode(nullCurrent ? "dark" : "light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Composer window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(encryptedUiItem(window, "encryptedReadStatus"));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QVERIFY(!f.controller.property("canCompose").toBool()); QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        auto *composer = encryptedUiItem(window, "messageComposer"); QVERIFY(!composer || !composer->isEnabled());
        auto *send = encryptedUiItem(window, "sendButton"); QVERIFY(!send || !send->isEnabled());
        const int before = f.posts; QTest::keyClick(window, Qt::Key_Return); QCOMPARE(f.posts, before);
        for (const auto &posted : f.requestBodies) QVERIFY(posted.value("payload").toObject().value("kind").toString() != "markdown");
        QCOMPARE(f.controller.draft(), QString{});
        QVERIFY(!encryptedUiItem(window, "encryptedUnsupportedSendingNotice"));
        QCOMPARE(encryptedUiTextCount(window->contentItem(), nullCurrent
            ? QString("Отправка сообщений в защищённых чатах пока недоступна.")
            : QString("Sending encrypted messages is currently unavailable")), 0);
        if (!nullCurrent) {
            auto *request = encryptedUiItem(window, "requestCurrentChatKeyButton"); QVERIFY(request);
            QCOMPARE(request->height(), qreal(36));
            QCOMPARE(request->property("horizontalPadding").toReal(), qreal(16));
            auto *status = encryptedUiItem(window, "encryptedReadStatusText"); QVERIFY(status);
            QCOMPARE(status->property("lineHeight").toReal(), qreal(20));
        }
        const bool windowClosed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(windowClosed);
    }

    void uiOrdinaryComposerPreservesMarkdownAndAggregatePrompt_data() {
        QTest::addColumn<bool>("aggregate"); QTest::addColumn<bool>("closed");
        QTest::newRow("en-light-selected") << false << false;
        QTest::newRow("ru-dark-aggregate") << true << false;
        QTest::newRow("en-light-closed-selected") << false << true;
    }
    void uiOrdinaryComposerPreservesMarkdownAndAggregatePrompt() {
        QFETCH(bool, aggregate); QFETCH(bool, closed);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["encryption"] = false;
        f.stream["current_encryption_key_uuid"] = QJsonValue::Null; f.cipher = message("Existing ordinary history");
        useCompleteSendEcho(f); QVERIFY(f.start());
        if (aggregate) { f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy() && f.controller.messages().size() == 1); }
        if (closed) {
            auto event = cryptoEvent(QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Visible history"}, {"is_default", false}, {"is_done", true}});
            auto payload = event.value("payload").toObject(); payload["kind"] = "topic.updated"; event["payload"] = payload;
            QVERIFY(f.controller.applyRealtimeEvent(event)); QTRY_VERIFY(f.controller.topicClosed());
        }
        NativePreferences preferences(f.directory.filePath("composer-plain-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(aggregate ? "ru" : "en"); preferences.setMode(aggregate ? "dark" : "light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Composer window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        if (closed) {
            QTRY_VERIFY(encryptedUiItem(window, "topicClosedPrompt")); QVERIFY(!encryptedUiItem(window, "messageComposer"));
            QVERIFY(!f.controller.canSend()); QCOMPARE(f.posts, 0);
        } else if (aggregate) {
            QTRY_VERIFY(encryptedUiItem(window, "streamTopicPrompt")); QVERIFY(!encryptedUiItem(window, "messageComposer"));
            QVERIFY(!f.controller.canSend()); QCOMPARE(f.posts, 0);
        } else {
            QTRY_VERIFY(encryptedUiItem(window, "messageComposer")); auto *composer = encryptedUiItem(window, "messageComposer");
            QVERIFY(composer->isEnabled()); auto *send = encryptedUiItem(window, "sendButton"); QVERIFY(send); QVERIFY(!send->isEnabled());
            const QString text = "Synthetic ordinary composer control";
            encryptedUiClick(composer); QTRY_VERIFY(composer->hasActiveFocus());
            for (const auto key : text.toLatin1()) QTest::keyClick(window, key);
            QTRY_COMPARE(f.controller.draft(), text); QTRY_VERIFY(send->isEnabled());
            QSignalSpy clicked(send, SIGNAL(clicked())); QVERIFY(clicked.isValid());
            QSignalSpy sent(&f.controller, &NativeController::messageSent);
            encryptedUiClick(send); QTRY_COMPARE(clicked.size(), 1); QTRY_COMPARE(sent.size(), 1); QCOMPARE(f.posts, 1);
            QCOMPARE(f.requestBody.value("payload").toObject(), QJsonObject({{"kind", "markdown"}, {"content", text}}));
            QCOMPARE(f.requestBody.value("topic_uuid").toString(), topicId);
            const auto publicId = QUuid::createUuidV5(QUuid(topicId), f.requestBody.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
            QCOMPARE(sent.first().at(0).toString(), publicId);
            QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 1);
            QTRY_COMPARE(f.controller.draft(), QString{});
        }
        const bool windowClosed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(windowClosed);
    }

    void uiEncryptedComposerCheckAndRetryKeepFrozenCipher_data() {
        QTest::addColumn<bool>("applied");
        QTest::newRow("en-light-applied-check-only") << true;
        QTest::newRow("ru-dark-unapplied-explicit-retry") << false;
    }
    void uiEncryptedComposerCheckAndRetryKeepFrozenCipher() {
        QFETCH(bool, applied);
        Fixture f; f.stream = fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        const QString text = "Synthetic recoverable composer input";
        SourceSendPeer peer; QVERIFY(peer.prepare(f, topicId, text));
        const auto previous = f.http.handler; int encryptedPosts = 0;
        f.http.handler = [&, previous](const Request &request) {
            auto response = previous(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/messages/"
                && request.body.value("payload").toObject().value("kind").toString() == "e2ee" && ++encryptedPosts == 1) {
                if (!applied) f.savedRequests.remove(response.data.toObject().value("uuid").toString());
                response.status = 503; response.data = QJsonObject{};
            }
            return response;
        };
        const auto restoreHandler = qScopeGuard([&f, previous] { f.http.handler = previous; });
        NativePreferences preferences(f.directory.filePath("composer-recovery-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(applied ? "en" : "ru"); preferences.setMode(applied ? "light" : "dark");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Composer window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QTRY_VERIFY(encryptedUiItem(window, "messageComposer"));
        auto *composer = encryptedUiItem(window, "messageComposer"); encryptedUiClick(composer); QTRY_VERIFY(composer->hasActiveFocus());
        for (const auto key : text.toLatin1()) QTest::keyClick(window, key);
        QTRY_COMPARE(f.controller.draft(), text); auto *send = encryptedUiItem(window, "sendButton"); QVERIFY(send); QTRY_VERIFY(send->isEnabled());
        const auto before = f.posts; QSignalSpy sent(&f.controller, &NativeController::messageSent);
        encryptedUiClick(send); QTRY_COMPARE(f.posts, before + 1);
        const auto first = f.requestBody;
        const auto publicId = QUuid::createUuidV5(QUuid(topicId), first.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
        QTRY_COMPARE(f.controller.encryption().value("pendingSend").toMap().value("phase").toString(), QString("uncertain"));
        QCOMPARE(f.controller.encryption().value("pendingSend").toMap().value("id").toString(), publicId);
        QCOMPARE(f.controller.encryption().value("pendingSend").toMap().value("targetTopic").toString(), topicId);
        QCOMPARE(f.controller.draft(), text); QVERIFY(!send->isEnabled()); QCOMPARE(sent.size(), 0);
        QSignalSpy pendingFrame(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!pendingFrame.isEmpty());
        QTRY_VERIFY(encryptedUiItem(window, "checkPendingEncryptedSendButton"));
        auto *check = encryptedUiItem(window, "checkPendingEncryptedSendButton"); QVERIFY(check->isEnabled());
        QSignalSpy checkClicked(check, SIGNAL(clicked())); QVERIFY(checkClicked.isValid());
        const int getsBefore = exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId);
        encryptedUiClick(check); QTRY_COMPARE(checkClicked.size(), 1);
        QTRY_COMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), getsBefore + 1);
        QCOMPARE(f.posts, before + 1);
        if (!applied) {
            QTRY_VERIFY(!f.controller.encryption().value("pendingSend").toMap().value("busy").toBool());
            QCOMPARE(f.controller.encryption().value("pendingSend").toMap().value("id").toString(), publicId);
            QCOMPARE(f.controller.draft(), text);
            auto *retry = encryptedUiItem(window, "retryPendingEncryptedSendButton"); QVERIFY(retry); QTRY_VERIFY(retry->isEnabled());
            QSignalSpy retryClicked(retry, SIGNAL(clicked())); QVERIFY(retryClicked.isValid());
            encryptedUiClick(retry); QTRY_COMPARE(retryClicked.size(), 1); QTRY_COMPARE(f.posts, before + 2);
            QCOMPARE(f.requestBody, first);
        }
        QTRY_COMPARE(sent.size(), 1); QCOMPARE(sent.first().at(0).toString(), publicId);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), getsBefore + (applied ? 1 : 3));
        QTRY_VERIFY(f.controller.encryption().value("pendingSend").toMap().isEmpty()); QTRY_COMPARE(f.controller.draft(), QString{});
        QCOMPARE(encryptedPosts, applied ? 1 : 2);
        const auto verified = peer.verify(first.value("payload").toObject(), topicId);
        QVERIFY(verified.value("ok").toBool()); QVERIFY(verified.value("matchesExpected").toBool());
        QVERIFY(!QJsonDocument(first).toJson(QJsonDocument::Compact).contains(text.toUtf8()));
        const bool windowClosed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(windowClosed);
    }

    void uiEncryptedComposerCurrentKeyWithoutHistoryHasNoOutdatedNotice() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start()); QVERIFY(f.prepareA());
        f.stream["current_encryption_key_uuid"] = OtherGeneration;
        f.stream["updated_at"] = "2026-01-02T10:00:02.000Z";
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
        payload["kind"] = "stream.updated"; event["payload"] = payload; QVERIFY(f.controller.applyRealtimeEvent(event));
        f.controller.requestChatKey(OtherGeneration); QTRY_COMPARE(f.posts, 2);
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QTRY_COMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), OtherGeneration);
        QTRY_VERIFY(f.controller.property("canCompose").toBool());
        const auto rows = f.controller.messages();
        QVERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(displayed(f.controller, messageId).value("encryptionKeyUuid").toString(), returnedId);
        for (const auto &row : rows) {
            const auto message = row.toMap();
            QVERIFY(message.value("kind").toString() != "e2ee" || message.value("encryptionKeyUuid").toString() != OtherGeneration);
        }
        NativePreferences preferences(f.directory.filePath("composer-current-no-history-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Composer window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *composer = encryptedUiItem(window, "messageComposer"); QVERIFY(composer); QVERIFY(composer->isEnabled());
        QVERIFY(!window->property("encryptedSendingKeyOutdated").toBool());
        QVERIFY(!window->property("encryptedKeyNoticeVisible").toBool());
        QCOMPARE(encryptedUiTextCount(window->contentItem(), "The security key has changed. Request the key from the channel owner to read new messages"), 0);
        QVERIFY(!encryptedUiItem(window, "requestCurrentChatKeyButton"));
        QVERIFY(!encryptedUiItem(window, "encryptedUnsupportedSendingNotice"));
        auto *send = encryptedUiItem(window, "sendButton"); QVERIFY(send); QVERIFY(!send->isEnabled());
        QCOMPARE(f.controller.draft(), QString{}); QCOMPARE(f.posts, 2);
        const bool windowClosed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(windowClosed);
    }

    void uiEncryptedComposerClosedTopicKeepsDisabledSourceComposer() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start()); QVERIFY(f.prepareA());
        auto event = cryptoEvent(QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"name", "Visible history"}, {"is_default", false}, {"is_done", true}});
        auto payload = event.value("payload").toObject(); payload["kind"] = "topic.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event)); QTRY_VERIFY(f.controller.topicClosed());
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
        QVERIFY(!f.controller.property("canCompose").toBool());
        NativePreferences preferences(f.directory.filePath("composer-encrypted-closed-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Composer window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *composer = encryptedUiItem(window, "messageComposer"); QVERIFY(composer); QVERIFY(!composer->isEnabled());
        QVERIFY(!encryptedUiItem(window, "topicClosedPrompt"));
        auto *send = encryptedUiItem(window, "sendButton"); QVERIFY(send); QVERIFY(!send->isEnabled());
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        QCOMPARE(f.controller.selectedTopic(), topicId); QCOMPARE(f.posts, 1);
        const bool windowClosed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(windowClosed);
    }

    void uiEncryptedClosedHistoricalComposerKeepsKeyUpdateRequest() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start()); QVERIFY(f.prepareA());
        f.stream["current_encryption_key_uuid"] = OtherGeneration;
        f.stream["updated_at"] = "2026-01-02T10:00:02.000Z";
        auto changed = cryptoEvent(f.stream); auto streamPayload = changed.value("payload").toObject();
        streamPayload["kind"] = "stream.updated"; changed["payload"] = streamPayload;
        QVERIFY(f.controller.applyRealtimeEvent(changed));
        auto closed = cryptoEvent(QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"name", "Visible history"}, {"is_default", false}, {"is_done", true}});
        auto topicPayload = closed.value("payload").toObject(); topicPayload["kind"] = "topic.updated"; closed["payload"] = topicPayload;
        QVERIFY(f.controller.applyRealtimeEvent(closed)); QTRY_VERIFY(f.controller.topicClosed());
        // Wait for the already-required current-B missing lookup before reading its sending witness.
        QTRY_VERIFY(f.controller.encryption().value("canRequestCurrentKey").toBool());
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
        QCOMPARE(f.controller.encryption().value("currentKeyUuid").toString(), OtherGeneration);
        QVERIFY(!f.controller.property("canCompose").toBool());
        NativePreferences preferences(f.directory.filePath("composer-closed-historical-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Composer window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *composer = encryptedUiItem(window, "messageComposer"); QVERIFY(composer); QVERIFY(!composer->isEnabled());
        QVERIFY(window->property("encryptedSendingKeyOutdated").toBool());
        QVERIFY(window->property("encryptedKeyNoticeVisible").toBool());
        auto *notice = encryptedUiItem(window, "encryptedReadStatusText"); QVERIFY(notice);
        QCOMPARE(notice->property("text").toString(), QString("The security key has changed. Request the key from the channel owner to read new messages"));
        QVERIFY(!encryptedUiItem(window, "topicClosedPrompt"));
        auto *request = encryptedUiItem(window, "requestCurrentChatKeyButton"); QVERIFY(request); QVERIFY(request->isEnabled());
        QSignalSpy clicked(request, SIGNAL(clicked())); QVERIFY(clicked.isValid());
        encryptedUiClick(request); QTRY_COMPARE(clicked.size(), 1); QTRY_COMPARE(f.posts, 2);
        QCOMPARE(f.requestBody.value("payload").toObject().value("kind").toString(), QString("key_request"));
        QCOMPARE(f.requestBody.value("payload").toObject().value("key_uuid").toString(), OtherGeneration);
        QTRY_COMPARE(f.controller.encryption().value("state").toString(), QString("requested"));
        for (const auto &posted : f.requestBodies) QVERIFY(posted.value("payload").toObject().value("kind").toString() != "e2ee");
        QVERIFY(!composer->isEnabled()); QVERIFY(window->property("encryptedKeyNoticeVisible").toBool());
        auto *send = encryptedUiItem(window, "sendButton"); QVERIFY(send); QVERIFY(!send->isEnabled());
        const bool windowClosed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(windowClosed);
    }

    // SOURCE-ONLY additive proposal. Run AFTER the two pending-cache transition
    // fixes, so a notification failure is not hidden by the earlier setup RED.
    // Reuses the sibling's fourteen response rows/helpers; no Main/test edit.
    void uiRejectedKeyResponseRefreshesAfterFreshIssuerAuthority() {
        Fixture f;
        f.stream = fullEncryptedStream();
        f.stream["owner"] = userId;
        f.stream["role"] = "owner";
        f.issuerRole = "administrator";
        const auto roles = addSourceRecipientBinding(f);
        useCompleteSendEcho(f);
        QVERIFY(f.start());
        f.controller.selectStream(streamId);
        QTRY_VERIFY(!f.controller.busy());
        f.controller.requestChatKey(returnedId);
        QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(roles->value(userId), QString("owner"));
        QCOMPARE(roles->value(Issuer), QString("administrator"));
        const auto own = f.savedRequest;
        const auto requestId = own.value("uuid").toString();
        QVERIFY(!requestId.isEmpty());
        QVERIFY(own.value("read").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        const auto source = f.sourceResponse(true);
        QVERIFY(source.value("ok").toBool());
        const auto response = f.responseDto(source);
        const auto responseId = response.value("uuid").toString();
        QVERIFY(!responseId.isEmpty());
        QVERIFY(response.value("read").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(response)));
        QTRY_VERIFY(f.vault.requestCompletionByMessage(f.vault.lease(), {streamId, returnedId}, requestId).value.has_value());
        QCOMPARE(f.vault.requestCompletionByMessage(f.vault.lease(), {streamId, returnedId}, requestId).value->outcome,
                 NativeChatKeyVault::Outcome::Rejected);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());

        NativePreferences preferences(f.directory.filePath("response-authority-notify-ui.ini"));
        preferences.setOwner(f.controller.ownerKey());
        preferences.setLocale("en");
        preferences.setMode("light");
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data()))
                QTest::qFail("Authority response window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
        window->update();
        QTRY_VERIFY(!rendered.isEmpty());

        expandInitialServiceGroup(window,responseId); QVERIFY(!QTest::currentTestFailed());
        const auto requestButton = [&]() {
            return encryptedUiItem(window, "requestRejectedChatKeyButton-" + responseId);
        };
        const auto requestUnavailable = [&]() {
            const auto *item = requestButton();
            return !item || !item->isEnabled();
        };
        // Read the real Main delegate only. In particular, do not invoke
        // controller.messages()/displayed() to refresh or assert these rows.
        QTRY_VERIFY(requestButton() && requestButton()->isEnabled());
        QVERIFY(encryptedUiItem(window, "keyServiceResponse-" + responseId));
        QCOMPARE(requestButton()->property("text").toString(), QString("Request"));
        const auto historyGets = [&]() {
            return std::count_if(f.http.requests.cbegin(), f.http.requests.cend(), [](const Request &request) {
                return request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/";
            });
        };
        const auto initialHistoryGets = historyGets();
        const auto posts = f.posts;
        const auto owner = f.controller.ownerKey();
        const auto detailPath = "/api/workspace/v1/messenger/streams/" + streamId;
        const auto activeDetail = [&]() -> QNetworkReply * {
            for (auto *reply : f.api.findChildren<QNetworkReply *>())
                if (reply->url().path() == detailPath && !reply->isFinished()) return reply;
            return nullptr;
        };
        const auto issuerRoleEvent = [](const QString &role) {
            auto event = cryptoEvent(QJsonObject{{"uuid", returnedId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"user_uuid", Issuer}, {"who_uuid", userId},
                {"role", role}, {"notification_mode", "all_messages"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:01:00Z"}});
            auto payload = event.value("payload").toObject();
            payload["kind"] = "stream_binding.updated";
            event["payload"] = payload;
            return event;
        };
        QSignalSpy modelNotifications(&f.controller, &NativeController::messagesChanged);

        // First refresh: same owner/device/request, but the actual issuer is no
        // longer a manager. Fixture owner is the VIEWER, so no owner bypass.
        auto details = f.streamGets, bindings = f.bindingGets, topics = f.topicGets;
        f.streamDelay = 1200; // Existing controlled delay; no wait/timeout growth.
        f.issuerRole = "member";
        QVERIFY(f.controller.applyRealtimeEvent(issuerRoleEvent("member")));
        QTRY_COMPARE(f.streamGets, details + 1);
        QPointer<QNetworkReply> demotionDetail = activeDetail();
        QVERIFY2(demotionDetail, "Fresh demotion authority detail must be positively in flight");
        QCOMPARE(f.bindingGets, bindings);
        QTRY_VERIFY(requestUnavailable());
        QVERIFY2(demotionDetail && !demotionDetail->isFinished(), "Request must stop admitting input before delayed authority completes");
        QTRY_COMPARE(f.bindingGets, bindings + 1);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(roles->value(userId), QString("owner"));
        QCOMPARE(roles->value(Issuer), QString("member"));
        QCOMPARE(f.streamGets, details + 1);
        QCOMPARE(f.topicGets, topics + 1);
        QVERIFY(requestUnavailable());
        QCOMPARE(f.posts, posts);
        QCOMPARE(historyGets(), initialHistoryGets);

        // Second refresh: a payload saying administrator alone must not restore
        // the action. Only the new complete owned authority may publish it.
        details = f.streamGets; bindings = f.bindingGets; topics = f.topicGets;
        f.issuerRole = "administrator";
        QVERIFY(f.controller.applyRealtimeEvent(issuerRoleEvent("administrator")));
        QTRY_COMPARE(f.streamGets, details + 1);
        QPointer<QNetworkReply> restorationDetail = activeDetail();
        QVERIFY2(restorationDetail, "Fresh restoration authority detail must be positively in flight");
        QCOMPARE(f.bindingGets, bindings);
        QVERIFY(requestUnavailable());
        const auto notificationsWhileWaiting = modelNotifications.size();
        QTRY_COMPARE(f.bindingGets, bindings + 1);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(roles->value(userId), QString("owner"));
        QCOMPARE(roles->value(Issuer), QString("administrator"));
        QCOMPARE(f.streamGets, details + 1);
        QCOMPARE(f.topicGets, topics + 1);
        QTRY_VERIFY(requestButton() && requestButton()->isEnabled());
        QTRY_VERIFY(modelNotifications.size() > notificationsWhileWaiting);
        QCOMPARE(requestButton()->property("text").toString(), QString("Request"));
        auto *service = encryptedUiItem(window, "keyServiceResponse-" + responseId);
        QVERIFY(service);
        QVERIFY(encryptedUiText(service, QString("You can request the key again")));
        QCOMPARE(f.controller.ownerKey(), owner);
        QCOMPARE(f.controller.selectedStream(), streamId);
        QVERIFY(f.controller.selectedTopic().isEmpty());
        QCOMPARE(f.posts, posts); // No action click; zero extra POST in BOTH waves.
        QCOMPARE(historyGets(), initialHistoryGets);
        const auto retainedReceipt = f.vault.requestCompletionByMessage(f.vault.lease(), {streamId, returnedId}, requestId);
        QVERIFY(retainedReceipt.value);
        QCOMPARE(retainedReceipt.value->outcome, NativeChatKeyVault::Outcome::Rejected);
        const bool closed = closeEncryptedVisualWindow(window);
        closeWindow.dismiss();
        QVERIFY(closed);
    }

    void uiReceivedKeyServiceResponseUsesSourceTextAndRequest_data() {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("grant"); QTest::addColumn<int>("viewportWidth");
        QTest::newRow("en-light-grant") << QString("en") << QString("light") << true << 1440;
        QTest::newRow("en-light-reject") << QString("en") << QString("light") << false << 1440;
        QTest::newRow("ru-dark-grant") << QString("ru") << QString("dark") << true << 1440;
        QTest::newRow("ru-dark-reject") << QString("ru") << QString("dark") << false << 1440;
        QTest::newRow("en-light-grant-700") << QString("en") << QString("light") << true << 700;
        QTest::newRow("ru-dark-reject-700") << QString("ru") << QString("dark") << false << 700;
    }
    void uiReceivedKeyServiceResponseUsesSourceTextAndRequest() {
        QFETCH(QString,locale); QFETCH(QString,mode); QFETCH(bool,grant); QFETCH(int,viewportWidth);
        Fixture f; f.stream=fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        QCOMPARE(f.controller.conversationKind(),QString("stream"));
        f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts,1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        const auto own=f.savedRequest, oldBody=f.requestBody;
        const auto oldRequestId=own.value("uuid").toString(); QVERIFY(!oldRequestId.isEmpty());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        const auto source=f.sourceResponse(!grant); QVERIFY(source.value("ok").toBool());
        auto response=f.responseDto(source);
        // Deliberately misleading presentation flag: canonical author is the
        // external Source issuer. The response grammar must not use is_own.
        response["is_own"]=true;
        const auto id=response.value("uuid").toString(); QVERIFY(!id.isEmpty());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(response)));
        if (grant) {
            f.cipher["payload"]=source.value("encrypted");
            QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
            QTRY_VERIFY(displayed(f.controller,messageId).value("decrypted").toBool());
            QCOMPARE(displayed(f.controller,messageId).value("content").toString(),QString("Synthetic source-granted plaintext"));
        }
        QTRY_VERIFY(displayed(f.controller,id).contains("keyServiceResponse"));
        QTRY_VERIFY(displayed(f.controller,id).value("keyServiceResponse").toMap().value(grant?"hasKey":"canRequest").toBool());
        const auto facts=displayed(f.controller,id).value("keyServiceResponse").toMap();
        QCOMPARE(facts.size(),11); QCOMPARE(facts.value("requestId").toString(),oldRequestId);
        QCOMPARE(facts.value("keyUuid").toString(),returnedId); QCOMPARE(facts.value("senderName").toString(),QString("#aaaaaaaa"));
        QVERIFY(facts.value("recipientKnown").toBool()); QCOMPARE(facts.value("recipientId").toString(),userId);
        QCOMPARE(facts.value("deviceName").toString(),oldBody.value("payload").toObject().value("device_name").toString());
        QVERIFY(facts.value("ownCurrentDevice").toBool()); QCOMPARE(facts.value("hasKey").toBool(),grant);
        QCOMPARE(facts.value("offerRequest").toBool(),!grant); QCOMPARE(facts.value("canRequest").toBool(),!grant);
        for (const auto *key:{"payload","content","deviceId","publicKey","privateKey"}) QVERIFY(!facts.contains(key));
        const auto receipt=f.vault.requestCompletionByMessage(f.vault.lease(),{streamId,returnedId},oldRequestId);
        QVERIFY(receipt.value); QCOMPARE(receipt.value->outcome,grant?NativeChatKeyVault::Outcome::Granted:NativeChatKeyVault::Outcome::Rejected);
        NativePreferences preferences(f.directory.filePath("received-response-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        QCOMPARE(preferences.locale(),locale); QCOMPARE(preferences.mode(),mode);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow=qScopeGuard([window=QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Received response window did not close after inactive focus",__FILE__,__LINE__);
        });
        QCOMPARE(window->minimumWidth(),700); window->resize(viewportWidth,900);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QTRY_COMPARE(window->size(),QSize(viewportWidth,900)); QCOMPARE(window->devicePixelRatio(),qreal(1));
        QCOMPARE(window->rendererInterface()->graphicsApi(),QSGRendererInterface::OpenGL);
        expandInitialServiceGroup(window,id); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyServiceResponse-"+id); QVERIFY(service);
        auto *title=encryptedUiItem(window,"keyServiceResponseText-"+id); QVERIFY(title);
        const auto device=facts.value("deviceName").toString();
        const auto expected=grant?(locale=="ru"?QString("#aaaaaaaa передал(а) вам ключ безопасности для устройства «%1»"):QString("#aaaaaaaa sent you a security key for device “%1”"))
            :(locale=="ru"?QString("#aaaaaaaa отклонил(а) ваш запрос ключа безопасности для устройства «%1»"):QString("#aaaaaaaa rejected your security key request for device “%1”"));
        QCOMPARE(title->property("text").toString(),expected.arg(device));
        QCOMPARE(title->property("textFormat").toInt(),int(Qt::PlainText));
        QCOMPARE(title->property("horizontalAlignment").toInt(),int(Qt::AlignHCenter));
        // Matched actual Source28: all four1440px response titles fit one20px line.
        // Native must not truncate the fractional natural width into a second line.
        if (viewportWidth == 1440) {
            QCOMPARE(title->property("lineCount").toInt(),1);
            QCOMPARE(title->height(),qreal(20));
        } else {
            QVERIFY(title->property("lineCount").toInt()>1);
            QVERIFY(title->height()>20);
        }
        QVERIFY(title->property("contentWidth").toReal()<=title->width()+1);
        QVERIFY(title->property("contentHeight").toReal()<=title->height()+1);
        auto *icon=encryptedUiItem(window,"keyServiceResponseIcon-"+id); QVERIFY(icon);
        QCOMPARE(icon->size(),QSizeF(24,24)); QCOMPARE(icon->property("glyphSize").toInt(),24);
        QCOMPARE(icon->property("asset").toString(),grant?QString("key-granted.svg"):QString("key-rejected.svg"));
        QVERIFY(!icon->isEnabled());
        const QRectF serviceRect(service->mapToScene(QPointF{}),service->size());
        const QRectF titleRect(title->mapToScene(QPointF{}),title->size());
        const QRectF iconRect(icon->mapToScene(QPointF{}),icon->size());
        QVERIFY(serviceRect.contains(titleRect)); QVERIFY(serviceRect.contains(iconRect));
        QVERIFY(QRectF(QPointF{},QSizeF(window->size())).contains(serviceRect));
        QVERIFY(qAbs(titleRect.left()-iconRect.right()-4)<=1);
        const auto readyText=locale=="ru"?QString("Ключ доступен на этом устройстве"):QString("The key is available on this device");
        captureEncryptedUi(window,locale+'-'+mode+(viewportWidth==700?"-700":""),grant?"received-key-grant":"received-key-reject",oldRequestId,id,facts);
        const auto before=f.posts; const auto draft=f.controller.draft();
        if (grant) {
            auto *ready=encryptedUiText(service,readyText); QVERIFY(ready);
            QCOMPARE(ready->property("textFormat").toInt(),int(Qt::PlainText));
            QVERIFY(!encryptedUiItem(window,"requestRejectedChatKeyButton-"+id));
            QCOMPARE(f.posts,before);
        } else {
            QVERIFY(!encryptedUiText(service,readyText));
            QVERIFY(encryptedUiText(service,locale=="ru"?QString("Вы можете запросить ключ повторно"):QString("You can request the key again")));
            auto *request=encryptedUiItem(window,"requestRejectedChatKeyButton-"+id); QVERIFY(request); QVERIFY(request->isEnabled());
            QVERIFY(serviceRect.contains(QRectF(request->mapToScene(QPointF{}),request->size())));
            QCOMPARE(request->property("text").toString(),locale=="ru"?QString("Запросить"):QString("Request"));
            QCOMPARE(request->height(),qreal(20)); QCOMPARE(request->property("horizontalPadding").toReal(),qreal(0));
            QCOMPARE(request->property("verticalPadding").toReal(),qreal(0));
            const auto font=QQmlProperty(request,"font").read().value<QFont>();
            QCOMPARE(font.pixelSize(),14); QCOMPARE(font.weight(),QFont::Normal); QVERIFY(font.underline());
            auto *content=request->property("contentItem").value<QQuickItem *>(); QVERIFY(content);
            QCOMPARE(content->property("textFormat").toInt(),int(Qt::PlainText));
            QSignalSpy clicked(request,SIGNAL(clicked())); QVERIFY(clicked.isValid());
            encryptedUiClick(request); QTRY_COMPARE(clicked.size(),1); QTRY_COMPARE(f.posts,before+1);
            QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
            const auto body=f.requestBody,payload=body.value("payload").toObject();
            QCOMPARE(body.value("stream_uuid").toString(),streamId); QCOMPARE(body.value("topic_uuid").toString(),secondTopicId);
            QCOMPARE(payload.value("kind").toString(),QString("key_request")); QCOMPARE(payload.value("key_uuid").toString(),returnedId);
            QVERIFY(!payload.contains("content")); QVERIFY(!payload.contains("request_message_uuid"));
            QVERIFY(body.value("uuid").toString()!=oldBody.value("uuid").toString());
            const auto canonical=QUuid::createUuidV5(QUuid(secondTopicId),body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
            QVERIFY(canonical!=oldRequestId); QCOMPARE(f.savedRequest.value("uuid").toString(),canonical);
            auto pending=f.vault.pendingRequests(f.vault.lease(),{streamId,returnedId});
            const auto clearPending=qScopeGuard([&] {for (auto &item:pending.values) item.clearSecrets();});
            QCOMPARE(pending.failure,NativeChatKeyVault::Failure::None); QCOMPARE(pending.values.size(),1);
            QCOMPARE(pending.values.first().localId,body.value("uuid").toString()); QCOMPARE(pending.values.first().messageId,canonical);
            // A full matching canonical POST echo may bind without GET. Do not
            // add a transport round trip to the existing successful request path.
            QTRY_VERIFY(!displayed(f.controller,id).value("keyServiceResponse").toMap().value("canRequest").toBool());
            auto *disabled=encryptedUiItem(window,"requestRejectedChatKeyButton-"+id); QVERIFY(disabled); QVERIFY(!disabled->isEnabled());
            QCOMPARE(disabled->property("text").toString(),locale=="ru"?QString("Запросить"):QString("Request"));
            QVERIFY(disabled->window()==window); QVERIFY(disabled->width()>0 && disabled->height()>0);
            QSignalSpy disabledClicked(disabled,SIGNAL(clicked())); QVERIFY(disabledClicked.isValid());
            // The common enabled-only helper cannot target this control. Send
            // the second real pointer click to the same window and disabled row.
            QTest::mouseClick(window,Qt::LeftButton,{},disabled->mapToScene(QPointF(disabled->width()/2,disabled->height()/2)).toPoint());
            QCOMPARE(disabledClicked.size(),0); QCOMPARE(f.posts,before+1);
            const auto retainedReceipt=f.vault.requestCompletionByMessage(f.vault.lease(),{streamId,returnedId},oldRequestId);
            QVERIFY(retainedReceipt.value); QCOMPARE(retainedReceipt.value->outcome,NativeChatKeyVault::Outcome::Rejected);
        }
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts,before+(grant?0:1)); QCOMPARE(f.controller.draft(),draft);
        const bool closed=closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }

    void uiKeyServiceResponseHasNoActionWithoutCurrentRecipient_data() {
        QTest::addColumn<bool>("knownRecipient"); QTest::addColumn<bool>("grant");
        QTest::newRow("same-account-other-device-grant") << true << true;
        QTest::newRow("same-account-other-device-reject") << true << false;
        QTest::newRow("unknown-request-grant") << false << true;
        QTest::newRow("unknown-request-reject") << false << false;
    }
    void uiKeyServiceResponseHasNoActionWithoutCurrentRecipient() {
        QFETCH(bool,knownRecipient); QFETCH(bool,grant);
        Fixture f; f.stream=fullEncryptedStream(); useCompleteSendEcho(f); QVERIFY(f.start());
        f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        auto pair=NativeChatCrypto::createRequestKeyPair(); QVERIFY(pair);
        const auto clearPair=qScopeGuard([&] {pair.privateKey.fill(0); pair.privateKey.clear();});
        const auto local=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto requestId=QUuid::createUuidV5(QUuid(secondTopicId),local.toUtf8()).toString(QUuid::WithoutBraces);
        const auto deviceId=knownRecipient?SourceRecipientDevice:f.vault.device().id;
        const QString deviceName="Synthetic other device";
        const QJsonObject payload{{"kind","key_request"},{"key_uuid",returnedId},{"device_id",deviceId},
            {"device_name",deviceName},{"public_key",QString::fromLatin1(pair.publicKey.toBase64())}};
        auto own=message({},requestId,secondTopicId); own["payload"]=payload;
        const QJsonObject body{{"uuid",local},{"stream_uuid",streamId},{"topic_uuid",secondTopicId},{"payload",payload}};
        if (knownRecipient) QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        const auto source=f.sourceResponseFor(body,own,!grant); QVERIFY(source.value("ok").toBool());
        const auto response=f.responseDto(source); const auto id=response.value("uuid").toString();
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(response)));
        QTRY_VERIFY(displayed(f.controller,id).contains("keyServiceResponse"));
        const auto facts=displayed(f.controller,id).value("keyServiceResponse").toMap();
        QCOMPARE(facts.value("recipientKnown").toBool(),knownRecipient);
        if (knownRecipient) QCOMPARE(facts.value("recipientId").toString(),userId);
        QVERIFY(!facts.value("ownCurrentDevice").toBool()); QVERIFY(!facts.value("hasKey").toBool());
        QVERIFY(!facts.value("offerRequest").toBool()); QVERIFY(!facts.value("canRequest").toBool());
        NativePreferences preferences(f.directory.filePath("response-negative-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow=qScopeGuard([window=QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Response negative window did not close after inactive focus",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); QSignalSpy rendered(window,&QQuickWindow::frameSwapped);
        window->update(); QTRY_VERIFY(!rendered.isEmpty());
        expandInitialServiceGroup(window,id); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyServiceResponse-"+id); QVERIFY(service);
        auto *title=encryptedUiItem(window,"keyServiceResponseText-"+id); QVERIFY(title);
        const auto expected=knownRecipient?(grant?QString("#aaaaaaaa sent you a security key for device “Synthetic other device”")
            :QString("#aaaaaaaa rejected your security key request for device “Synthetic other device”"))
            :(grant?QString("#aaaaaaaa sent an encrypted security key Recipient information is unavailable.")
                :QString("#aaaaaaaa rejected the key request Recipient information is unavailable."));
        QCOMPARE(title->property("text").toString(),expected); QCOMPARE(title->property("textFormat").toInt(),int(Qt::PlainText));
        auto *icon=encryptedUiItem(window,"keyServiceResponseIcon-"+id); QVERIFY(icon); QCOMPARE(icon->size(),QSizeF(24,24));
        QVERIFY(!encryptedUiItem(window,"requestRejectedChatKeyButton-"+id));
        QVERIFY(!encryptedUiText(service,QString("The key is available on this device")));
        QVERIFY(!encryptedUiText(service,QString("You can request the key again")));
        auto key=f.vault.read(f.vault.lease(),{streamId,returnedId});
        const auto clearKey=qScopeGuard([&] {key.value.bytes.fill(0); key.value.bytes.clear();});
        QCOMPARE(key.failure,NativeChatKeyVault::Failure::None); QVERIFY(key.value.bytes.isEmpty());
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts,0);
        const bool closed=closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }

    void uiKeyServiceResponseUsesCanonicalSelfAndOutgoingGrammar_data() {
        QTest::addColumn<QString>("grammar"); QTest::addColumn<bool>("grant");
        QTest::newRow("self-grant") << QString("self") << true;
        QTest::newRow("self-reject") << QString("self") << false;
        QTest::newRow("outgoing-grant") << QString("outgoing") << true;
        QTest::newRow("outgoing-reject") << QString("outgoing") << false;
        QTest::newRow("external-other-grant") << QString("external-other") << true;
        QTest::newRow("external-other-reject") << QString("external-other") << false;
    }
    void uiKeyServiceResponseUsesCanonicalSelfAndOutgoingGrammar() {
        QFETCH(QString,grammar); QFETCH(bool,grant);
        const bool selfRecipient=grammar=="self",ownSender=grammar!="external-other";
        Fixture f; f.stream=fullEncryptedStream(); f.stream["role"]="administrator";
        const auto roles=addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        auto pair=NativeChatCrypto::createRequestKeyPair(); QVERIFY(pair);
        const auto clearPair=qScopeGuard([&] {pair.privateKey.fill(0); pair.privateKey.clear();});
        const auto local=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto requestId=QUuid::createUuidV5(QUuid(secondTopicId),local.toUtf8()).toString(QUuid::WithoutBraces);
        const QString deviceName="Synthetic other device";
        const QJsonObject payload{{"kind","key_request"},{"key_uuid",returnedId},{"device_id",SourceRecipientDevice},
            {"device_name",deviceName},{"public_key",QString::fromLatin1(pair.publicKey.toBase64())}};
        auto own=message({},requestId,secondTopicId); own["payload"]=payload;
        own["author_uuid"]=selfRecipient?userId:SourceRecipient; own["is_own"]=false;
        const QJsonObject body{{"uuid",local},{"stream_uuid",streamId},{"topic_uuid",secondTopicId},{"payload",payload}};
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        const auto source=f.sourceResponseFor(body,own,!grant); QVERIFY(source.value("ok").toBool());
        auto response=f.responseDto(source);
        // Source-generated response payload plus controlled admitted canonical
        // sender DTO: grammar coverage, not Source self-delivery/issuance E2E.
        response["author_uuid"]=ownSender?userId:Issuer; response["is_own"]=!ownSender;
        const auto id=response.value("uuid").toString(); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(response)));
        QTRY_VERIFY(displayed(f.controller,id).value("keyServiceResponse").toMap().value("recipientKnown").toBool());
        // The role witness is filled by the real binding response, after selection
        // and admitted controls schedule shared authority, never during f.start().
        QTRY_COMPARE(roles->value(userId),QString("administrator"));
        const auto facts=displayed(f.controller,id).value("keyServiceResponse").toMap();
        QCOMPARE(displayed(f.controller,id).value("authorId").toString(),ownSender?userId:Issuer);
        QCOMPARE(facts.value("recipientId").toString(),selfRecipient?userId:SourceRecipient);
        QVERIFY(!facts.value("ownCurrentDevice").toBool()); QVERIFY(!facts.value("offerRequest").toBool());
        NativePreferences preferences(f.directory.filePath("response-grammar-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow=qScopeGuard([window=QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Response grammar window did not close after inactive focus",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); QSignalSpy rendered(window,&QQuickWindow::frameSwapped);
        window->update(); QTRY_VERIFY(!rendered.isEmpty());
        expandInitialServiceGroup(window,id); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyServiceResponse-"+id); QVERIFY(service);
        auto *title=encryptedUiItem(window,"keyServiceResponseText-"+id); QVERIFY(title);
        const auto expected=selfRecipient?(grant?QString("You sent a security key for device “Synthetic other device”")
            :QString("You rejected the security key request for device “Synthetic other device”"))
            :ownSender?(grant?QString("You sent a security key to #eeeeeeee for device “Synthetic other device”")
                :QString("You rejected the security key request from #eeeeeeee for device “Synthetic other device”"))
            :(grant?QString("#aaaaaaaa sent a security key to #eeeeeeee for device “Synthetic other device”")
                :QString("#aaaaaaaa rejected the security key request from #eeeeeeee for device “Synthetic other device”"));
        QCOMPARE(title->property("text").toString(),expected); QCOMPARE(title->property("textFormat").toInt(),int(Qt::PlainText));
        QVERIFY(!encryptedUiItem(window,"requestRejectedChatKeyButton-"+id));
        QVERIFY(!encryptedUiText(service,QString("The key is available on this device")));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts,0);
        const bool closed=closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }
    void uiIncomingKeyRequestCentersWrappedLinesAtMinimumWidth_data() {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::newRow("en-light") << QString("en") << QString("light");
        QTest::newRow("en-dark") << QString("en") << QString("dark");
        QTest::newRow("ru-light") << QString("ru") << QString("light");
        QTest::newRow("ru-dark") << QString("ru") << QString("dark");
    }
    void uiIncomingKeyRequestCentersWrappedLinesAtMinimumWidth() {
        QFETCH(QString,locale); QFETCH(QString,mode);
        Fixture f; f.stream=fullEncryptedStream(); f.stream["role"]="administrator";
        const auto roles=addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f,true));
        QCOMPARE(roles->value(userId),QString("administrator"));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY(incomingKeyRequestRow(f.controller,peer.requestId).value("canGrant").toBool());
        const auto request=incomingKeyRequestRow(f.controller,peer.requestId);
        QCOMPARE(request.value("authorId").toString(),SourceRecipient); QVERIFY(!request.value("name").toString().isEmpty());
        QVERIFY(request.value("offerGrant").toBool()); QVERIFY(request.value("offerReject").toBool());
        QVERIFY(!request.value("ownCurrentDevice").toBool());
        const auto postsBefore=f.posts; const auto draftBefore=f.controller.draft();
        NativePreferences preferences(f.directory.filePath("incoming-wrap-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        QCOMPARE(preferences.locale(),locale); QCOMPARE(preferences.mode(),mode);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow=qScopeGuard([window=QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Wrapped request window did not close after inactive focus",__FILE__,__LINE__);
        });
        QCOMPARE(window->minimumWidth(),700); window->resize(700,900);
        QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_COMPARE(window->size(),QSize(700,900));
        QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->devicePixelRatio(),qreal(1));
        QCOMPARE(window->rendererInterface()->graphicsApi(),QSGRendererInterface::OpenGL);
        expandInitialServiceGroup(window,peer.requestId); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyRequestService-"+peer.requestId); QVERIFY(service);
        const auto device=peer.requestDto.value("payload").toObject().value("device_name").toString().trimmed(); QVERIFY(!device.isEmpty());
        const auto titleText=(locale=="ru"?QString("запросил(а) ключ безопасности для устройства «%1»")
            :QString("requested a security key for device “%1”")).arg(device);
        auto *title=encryptedUiText(service,titleText); QVERIFY(title);
        auto *actor=encryptedUiText(service,request.value("name").toString()); QVERIFY(actor);
        auto *grant=encryptedUiItem(window,"grantChatKeyButton-"+peer.requestId); QVERIFY(grant);
        auto *reject=encryptedUiItem(window,"rejectChatKeyButton-"+peer.requestId); QVERIFY(reject);
        QVERIFY(grant->isEnabled()); QVERIFY(reject->isEnabled());
        QCOMPARE(grant->property("text").toString(),locale=="ru"?QString("Передать ключ"):QString("Send key"));
        QCOMPARE(reject->property("text").toString(),locale=="ru"?QString("Отклонить"):QString("Reject"));
        QVERIFY(title->property("lineCount").isValid());
        const auto titleLines=title->property("lineCount").toInt();
        const bool wrapped=titleLines>1 && title->height()>20;
        const bool titleCentered=title->property("horizontalAlignment").toInt()==int(Qt::AlignHCenter);
        bool plainText=true,contained=service->width()>0 && service->height()>0;
        struct Line { qreal centerY,left,right; QStringList parts; };
        QList<Line> lines; QJsonArray parts;
        const QList<QPair<QString,QQuickItem *>> items={{"actor",actor},{"title",title},{"grant",grant},{"reject",reject}};
        for (const auto &part:items) {
            auto *item=part.second; QCOMPARE(item->window(),window); QVERIFY(item->isVisible());
            auto *textItem=item;
            if (part.first=="grant" || part.first=="reject") {
                textItem=item->property("contentItem").value<QQuickItem *>(); QVERIFY(textItem);
            }
            QVERIFY(textItem->property("textFormat").isValid());
            plainText=plainText && textItem->property("textFormat").toInt()==int(Qt::PlainText);
            QVERIFY(textItem->property("contentWidth").isValid()); QVERIFY(textItem->property("contentHeight").isValid());
            const auto textWidth=textItem->property("contentWidth").toReal(),textHeight=textItem->property("contentHeight").toReal();
            contained=contained && textWidth>0 && textHeight>0 && textWidth<=textItem->width()+1 && textHeight<=textItem->height()+1;
            const auto point=item->mapToItem(service,QPointF{});
            const QRectF rect(point,item->size());
            contained=contained && rect.width()>0 && rect.height()>0 && rect.left()>=-1 && rect.top()>=-1
                && rect.right()<=service->width()+1 && rect.bottom()<=service->height()+1;
            int lineIndex=-1;
            for (qsizetype i=0;i<lines.size();++i) if (qAbs(lines[i].centerY-rect.center().y())<=1) {lineIndex=int(i);break;}
            if (lineIndex<0) {lineIndex=int(lines.size());lines.append(Line{rect.center().y(),rect.left(),rect.right(),{}});}
            auto &line=lines[lineIndex]; line.left=qMin(line.left,rect.left()); line.right=qMax(line.right,rect.right()); line.parts.append(part.first);
            parts.append(QJsonObject{{"part",part.first},{"rect",encryptedUiRect(item,service)},
                {"centerY",rect.center().y()},{"line",lineIndex},{"contentWidth",textWidth},{"contentHeight",textHeight}});
        }
        bool centered=true; QJsonArray lineMetrics;
        for (const auto &line:lines) {
            const auto offset=(line.left+line.right)/2-service->width()/2;
            centered=centered && qAbs(offset)<=1;
            lineMetrics.append(QJsonObject{{"centerY",line.centerY},{"left",line.left},{"right",line.right},
                {"centerOffset",offset},{"parts",QJsonArray::fromStringList(line.parts)}});
        }
        const auto sceneOrigin=service->mapToScene(QPointF{});
        const QRectF serviceScene(sceneOrigin,service->size());
        contained=contained && serviceScene.left()>=-1 && serviceScene.top()>=-1
            && serviceScene.right()<=window->width()+1 && serviceScene.bottom()<=window->height()+1;
        const auto metrics=QJsonDocument(QJsonObject{{"locale",locale},{"mode",mode},{"requestId",peer.requestId},
            {"deviceName",device},{"windowWidth",window->width()},{"windowHeight",window->height()},
            {"service",encryptedUiRect(service,window->contentItem())},{"titleLineCount",titleLines},
            {"titleAlignment",title->property("horizontalAlignment").toInt()},
            {"wrapped",wrapped},{"plainText",plainText},{"centered",centered},{"contained",contained},
            {"parts",parts},{"lines",lineMetrics}}).toJson(QJsonDocument::Compact);
        qInfo().noquote()<<"SOURCE_REQUEST_WRAP"<<metrics;
        // Complete the independent Source process without a grant/reject action,
        // before the intended layout assertion can return early on a RED row.
        const auto pending=peer.finish(); QVERIFY(pending.value("ok").toBool());
        QVERIFY(pending.value("recipientStillPending").toBool()); QVERIFY(!pending.value("recipientHasKey").toBool());
        QCOMPARE(pending.value("completionCount").toInt(),0);
        QCOMPARE(f.posts,postsBefore); QCOMPARE(f.controller.draft(),draftBefore);
        QCOMPARE(incomingKeyRequestRow(f.controller,peer.requestId).value("status").toString(),QString("pending"));
        QVERIFY2(wrapped,metrics.constData()); QVERIFY2(lines.size()>=2,metrics.constData());
        QVERIFY2(plainText,metrics.constData()); QVERIFY2(contained,metrics.constData());
        // The title is a multiline Text box: its own alignment centers each
        // internal line; flex-line box groups separately center actor/actions.
        captureEncryptedUi(window,locale+'-'+mode,"incoming-request-wrapped",peer.requestId);
        QVERIFY2(centered && titleCentered,metrics.constData());
        const bool closed=closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }

// Append inside the existing WORKSPACE_NATIVE_E2EE_UI slots only.
// Requires the reviewed OrdinaryRejectCase helper from the Core GREEN overlay.
    void uiOrdinaryManagerRejectsFromTheActualMainAction_data() {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("actualOwner");
        QTest::newRow("ordinary-owner-no-A-en-light") << QString("en") << QString("light") << true;
        QTest::newRow("ordinary-admin-no-A-ru-dark") << QString("ru") << QString("dark") << false;
    }
    void uiOrdinaryManagerRejectsFromTheActualMainAction() {
        QFETCH(QString, locale); QFETCH(QString, mode); QFETCH(bool, actualOwner);
        OrdinaryRejectCase c; auto &f = c.f;
        if (actualOwner) { f.stream["owner"] = userId; f.stream["role"] = "member"; }
        // Canonical metadata exists BEFORE this UI2's actual HTTP/history/apply.
        // Common OrdinaryRejectCase/Fixture/Core helpers are untouched.
        const auto canonicalUiRow=[](QJsonObject row) {
            if(row.value("author_uuid").toString().isEmpty()||row.value("created_at").toString().isEmpty()
                ||(row.contains("user_uuid")&&row.value("user_uuid").toString()!=userId))return QJsonObject{};
            const bool own=row.value("author_uuid").toString()==userId;
            if(row.contains("is_own")&&(!row.value("is_own").isBool()||row.value("is_own").toBool()!=own))return QJsonObject{};
            row["user_uuid"]=userId;row["is_own"]=own;
            if(!row.contains("updated_at"))row["updated_at"]=row.value("created_at");
            if(!row.contains("pinned"))row["pinned"]=false;
            for(const auto *field:{"reactions","reaction_users"})if(!row.contains(field))row[field]=QJsonObject{};
            return row;
        };
        f.cipher=canonicalUiRow(f.cipher);QVERIFY(!f.cipher.isEmpty());
        const bool publicCapture=!qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY").isEmpty();
        QHash<QString,QJsonValue> publicSnapshots;QJsonArray publicObservations;
        QJsonObject actualPublicPost,actualCanonicalGet;bool publicObservationOverflow=false;
        const auto previousPublicHandler=f.http.handler;
        const auto restorePublicHandler=qScopeGuard([&f,previousPublicHandler]{f.http.handler=previousPublicHandler;});
        f.http.handler=[&](const Request &wire) {
            auto response=previousPublicHandler(wire);const auto path=wire.target.path();
            const bool post=wire.method=="POST"&&path=="/api/workspace/v1/messenger/messages/";
            const bool detail=wire.method=="GET"&&path.startsWith("/api/workspace/v1/messenger/messages/")&&!path.endsWith('/');
            if((post||detail)&&response.status>=200&&response.status<300&&response.data.isObject()) {
                const auto row=canonicalUiRow(response.data.toObject());response.data=row;
                f.savedRequests.insert(row.value("uuid").toString(),row);if(post)f.savedRequest=row;
            }
            if(publicCapture) {
                QJsonArray query;for(const auto &item:QUrlQuery(wire.target).queryItems())query.append(QJsonArray{item.first,item.second});
                QJsonObject observed{{"method",QString::fromLatin1(wire.method)},{"path",path},{"query",query},{"status",response.status},{"body",response.data}};
                bool record=false;
                if(wire.method=="GET"&&response.status==200) {
                    if(QStringList{"/api/workspace/v1/messenger/streams/","/api/workspace/v1/messenger/streams/"+streamId,
                        "/api/workspace/v1/messenger/stream_topics/","/api/workspace/v1/messenger/stream_bindings/","/api/workspace/v1/users/"}.contains(path)) {
                        publicSnapshots.insert(path,response.data);record=true;
                    } else if(path=="/api/workspace/v1/messenger/messages/"){publicSnapshots.insert("history",response.data);record=true;}
                }
                if(post){observed["requestBody"]=wire.body;actualPublicPost=observed;record=true;}
                if(detail&&response.status==200){actualCanonicalGet=observed;record=true;}
                if(record){if(publicObservations.size()<64)publicObservations.append(observed);else publicObservationOverflow=true;}
            }
            return response;
        };
        // Same three real start steps as c.start(), with R enrichment before RT.
        QVERIFY(f.start());QVERIFY(c.peer.prepare(f,false,OtherGeneration));
        c.peer.requestDto=canonicalUiRow(c.peer.requestDto);QVERIFY(!c.peer.requestDto.isEmpty());
        f.savedRequests.insert(c.peer.requestId,c.peer.requestDto);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(c.peer.requestDto)));
        QTRY_COMPARE(f.streamGets,1);QTRY_COMPARE(f.bindingGets,1);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()&&!c.row().isEmpty());
        QTRY_VERIFY(c.row().value("canReject").toBool());
        QCOMPARE(c.servedRoles->value(userId), actualOwner ? QString("member") : QString("administrator"));
        QCOMPARE(c.servedRoles->value(SourceRecipient), QString("member"));
        QVERIFY(!f.stream.value("encryption").toBool()); QVERIFY(f.stream.value("current_encryption_key_uuid").isNull());
        QVERIFY(!c.row().value("hasRequestedKey").toBool()); QVERIFY(!c.row().value("offerGrant").toBool());
        auto key = f.vault.read(f.vault.lease(), {streamId, OtherGeneration});
        const auto scrub = qScopeGuard([&] { key.value.bytes.fill(0); });
        QCOMPARE(key.failure, NativeChatKeyVault::Failure::None); QVERIFY(key.value.bytes.isEmpty());
        const auto posts = f.posts, details = f.streamGets, bindings = f.bindingGets, topics = f.topicGets;
        const auto oldDraft = f.controller.draft(); QCOMPARE(posts, 0);
        NativePreferences preferences(f.directory.filePath("ordinary-reject-happy-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Ordinary Reject window did not close", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        QVERIFY(!window->property("encryptedConversation").toBool());
        expandInitialServiceGroup(window, c.peer.requestId); QVERIFY(!QTest::currentTestFailed());
        auto *service = encryptedUiItem(window, "keyRequestService-" + c.peer.requestId); QVERIFY(service);
        auto *button = encryptedUiItem(window, "rejectChatKeyButton-" + c.peer.requestId); QVERIFY(button); QVERIFY(button->isEnabled());
        QCOMPARE(button->property("text").toString(), locale == "ru" ? QString("Отклонить") : QString("Reject"));
        QVERIFY(!encryptedUiItem(window, "grantChatKeyButton-" + c.peer.requestId));
        auto *list = encryptedUiItem(window, "messageList"); QVERIFY(list);
        const auto actionInsideViewport = [&] {
            auto *action = encryptedUiItem(window, "rejectChatKeyButton-" + c.peer.requestId);
            return action && action->window() == window && action->width() > 0 && action->height() > 0
                && QRectF(list->mapToScene(QPointF{}), list->size()).contains(QRectF(action->mapToScene(QPointF{}), action->size()));
        };
        QTRY_VERIFY(actionInsideViewport());
        const auto previousActionPoint = button->mapToScene(QPointF(button->width()/2, button->height()/2)).toPoint();
        QJsonObject publicPresentation,publicRoute;QJsonArray publicVisibleRows;
        const QString publicScenario="ordinary-reject-"+QString(actualOwner?"owner":"admin")+'-'+locale+'-'+mode;
        const QString publicPreStem="native-encrypted-"+publicScenario+"-pre-click-expanded";
        const auto publicFacts=[](const QVariantMap &row) {
            QJsonObject safe;
            for(const auto *field:{"id","authorId","keyUuid","name","deviceName","status"})safe[field]=row.value(field).toString();
            for(const auto *field:{"ownCurrentDevice","canManageKeys","hasRequestedKey","offerGrant","offerReject","canGrant","canReject","pending"})safe[field]=row.value(field).toBool();
            return safe;
        };
        if(publicCapture) {
            QCOMPARE(window->size(),QSize(1440,900));QCOMPARE(window->devicePixelRatio(),qreal(1));
            QCOMPARE(f.controller.selectedStream(),streamId);QCOMPARE(f.controller.selectedTopic(),topicId);
            QCOMPARE(f.controller.conversationKind(),QString("topic"));QVERIFY(f.controller.hasConversation());
            QVERIFY(f.controller.messageHistoryReady());
            publicPresentation=publicFacts(c.row());QCOMPARE(publicPresentation.value("status").toString(),QString("pending"));
            QVERIFY(publicPresentation.value("canManageKeys").toBool()&&publicPresentation.value("canReject").toBool());
            for(const auto &entry:f.controller.messages()) {
                const auto id=entry.toMap().value("id").toString();
                if(id==f.cipher.value("uuid").toString())publicVisibleRows.append(f.cipher);
                else if(id==c.peer.requestId)publicVisibleRows.append(c.peer.requestDto);
                else QFAIL("Unexpected ordinary capture row");
            }
            QCOMPARE(publicVisibleRows.size(),2);
            publicRoute={{"selectedStream",f.controller.selectedStream()},{"selectedTopic",f.controller.selectedTopic()},
                {"conversationKind",f.controller.conversationKind()},{"hasConversation",f.controller.hasConversation()},{"title",f.controller.title()}};
            captureEncryptedUi(window,publicScenario,"pre-click-expanded",c.peer.requestId);
        }
        QSignalSpy clicked(button, SIGNAL(clicked())); QVERIFY(clicked.isValid());
        encryptedUiClick(button); QTRY_COMPARE(clicked.size(), 1);
        QTRY_COMPARE(f.posts, posts + 1);
        QTRY_COMPARE(c.response().value("phase").toString(), QString("ready"));
        QTRY_COMPARE(c.row().value("status").toString(), QString("rejected"));
        const auto body = f.requestBody, payload = body.value("payload").toObject();
        const auto canonical = body.value("uuid").toString(); QVERIFY(isUuid(canonical));
        const auto publicId = QUuid::createUuidV5(QUuid(secondTopicId), canonical.toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(body.value("stream_uuid").toString(), streamId); QCOMPARE(body.value("topic_uuid").toString(), secondTopicId);
        QVERIFY(body.value("topic_uuid") != c.peer.requestDto.value("topic_uuid"));
        QCOMPARE(payload, (QJsonObject{{"kind", "key_reject"}, {"key_uuid", OtherGeneration},
            {"request_message_uuid", c.peer.requestId}, {"sender_device_id", f.vault.device().id}, {"recipient_device_id", SourceRecipientDevice}}));
        QCOMPARE(f.savedRequest.value("uuid").toString(), publicId);
        QCOMPARE(f.savedRequest.value("user_uuid").toString(), userId); QCOMPARE(f.savedRequest.value("author_uuid").toString(), userId);
        QCOMPARE(f.savedRequest.value("payload").toObject(), payload);
        QCOMPARE(exactRequests(f, "GET", "/api/workspace/v1/messenger/messages/" + publicId), 1);
        QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings); QCOMPARE(f.topicGets, topics);
        const auto result = c.peer.finish(f.savedRequest, canonical); QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("completionOutcome").toString(), QString("rejected"));
        QCOMPARE(result.value("completionRequest").toString(), c.peer.requestId); QCOMPARE(result.value("completionResponse").toString(), publicId);
        QCOMPARE(result.value("recipientPending").toInt(), 0); QVERIFY(!result.value("recipientHasKey").toBool());
        const auto terminal = locale == "ru" ? QString("Запрос отклонён.") : QString("Request rejected.");
        const auto terminalReady = [&] { auto *holder = encryptedUiItem(window, "keyRequestService-" + c.peer.requestId); return holder && encryptedUiText(holder, terminal); };
        QTRY_VERIFY(terminalReady());
        QVERIFY(!encryptedUiItem(window, "grantChatKeyButton-" + c.peer.requestId));
        QVERIFY(!encryptedUiItem(window, "rejectChatKeyButton-" + c.peer.requestId));
        if(publicCapture) {
            const auto completeUiRow=[](const QJsonObject &raw) {
                for(const auto *field:{"uuid","project_id","stream_uuid","topic_uuid","author_uuid","user_uuid","created_at","updated_at"})
                    if(!raw.value(field).isString()||raw.value(field).toString().isEmpty())return false;
                for(const auto *field:{"read","pinned","starred","is_own"})if(!raw.value(field).isBool())return false;
                return raw.value("payload").isObject()&&raw.value("reactions").isObject()&&raw.value("reaction_users").isObject();
            };
            QVERIFY(completeUiRow(f.cipher)&&completeUiRow(c.peer.requestDto)&&completeUiRow(f.savedRequest));
            QVERIFY(parseMessage(c.peer.requestDto,projectId));QVERIFY(parseMessage(f.cipher,projectId));
            QCOMPARE(actualPublicPost.value("requestBody").toObject(),f.requestBody);
            QCOMPARE(actualPublicPost.value("body").toObject(),f.savedRequest);QCOMPARE(actualCanonicalGet.value("body").toObject(),f.savedRequest);
            QCOMPARE(actualPublicPost.value("status").toInt(),201);QCOMPARE(actualCanonicalGet.value("status").toInt(),200);
            QVERIFY(!publicObservationOverflow);QCOMPARE(f.posts,1);
            const auto terminalFacts=publicFacts(c.row());QCOMPARE(terminalFacts.value("status").toString(),QString("rejected"));
            for(const auto *field:{"offerGrant","offerReject","canGrant","canReject","pending"})QVERIFY(!terminalFacts.value(field).toBool());
            QCOMPARE(f.controller.selectedStream(),publicRoute.value("selectedStream").toString());
            QCOMPARE(f.controller.selectedTopic(),publicRoute.value("selectedTopic").toString());
            const auto now=f.controller.messages();QCOMPARE(now.size(),publicVisibleRows.size());
            for(qsizetype i=0;i<now.size();++i)QCOMPARE(now[i].toMap().value("id").toString(),publicVisibleRows[i].toObject().value("uuid").toString());
            const auto snapshot=[&](const QString &path){const bool found=publicSnapshots.contains(path);return QJsonObject{{"observed",found},{"body",found?publicSnapshots.value(path):QJsonValue(QJsonValue::Null)}};};
            QJsonObject catalog;for(const auto &entry:publicSnapshots.value("/api/workspace/v1/messenger/streams/").toArray())
                if(entry.toObject().value("uuid").toString()==streamId)catalog=entry.toObject();
            QCOMPARE(catalog,f.stream);QCOMPARE(publicSnapshots.value("/api/workspace/v1/messenger/streams/"+streamId).toObject(),f.stream);
            const auto capturedTopics=publicSnapshots.value("/api/workspace/v1/messenger/stream_topics/");
            const auto capturedBindings=publicSnapshots.value("/api/workspace/v1/messenger/stream_bindings/");
            const auto capturedUsers=publicSnapshots.value("/api/workspace/v1/users/");
            QVERIFY(capturedTopics.isArray()&&capturedBindings.isArray()&&capturedUsers.isArray());
            QCOMPARE(publicSnapshots.value("history").toArray(),QJsonArray{f.cipher});
            QJsonArray visibleIds;QJsonObject labels{{c.peer.requestDto.value("author_uuid").toString(),publicPresentation.value("name")}};
            for(const auto &entry:publicVisibleRows){const auto raw=entry.toObject();visibleIds.append(raw.value("uuid"));labels[raw.value("author_uuid").toString()]=displayed(f.controller,raw.value("uuid").toString()).value("author").toString();}
            const auto device=c.peer.requestDto.value("payload").toObject().value("device_name").toString().trimmed();QVERIFY(!device.isEmpty());
            QJsonObject completion;for(const auto *field:{"ok","completionOutcome","completionRequest","completionResponse","recipientPending","recipientHasKey"})completion[field]=result.value(field);
            const QString publicTerminalStem="native-encrypted-"+publicScenario+"-post-canonical-terminal";
            captureEncryptedUi(window,publicScenario,"post-canonical-terminal",c.peer.requestId);
            QJsonObject pack{{"schema","native-ordinary-reject-public-v1"},{"synthetic",true},
                {"sourceProvenance",QJsonObject{{"actualNativeCase",QString::fromLatin1(QTest::currentTestFunction())+':'+QString::fromLatin1(QTest::currentDataTag())},
                    {"state","Actual public transport/presentation; external Root source/binary receipt; Source adapter is presentation-only"}}},
                {"ids",QJsonObject{{"project",projectId},{"user",userId},{"stream",streamId},{"topic",topicId},{"defaultTopic",secondTopicId},
                    {"key",OtherGeneration},{"request",c.peer.requestId},{"response",publicId},{"canonical",canonical}}},
                {"route",publicRoute},{"viewerName",f.controller.viewerDisplayName()},{"authorLabels",labels},
                {"request",c.peer.requestDto},{"response",f.savedRequest},{"messages",publicVisibleRows},{"catalog",catalog},
                {"topics",capturedTopics},{"bindings",capturedBindings},{"users",capturedUsers},
                {"presentation",publicPresentation},{"terminalPresentation",terminalFacts},
                {"publicState",QJsonObject{{"ordinaryNative",!f.stream.value("encryption").toBool()&&f.stream.value("source_name").toString()=="native"},
                    {"vaultAvailable",f.vault.available()},{"currentKeyNull",f.stream.value("current_encryption_key_uuid").isNull()},
                    {"requestedKeyReadOkay",key.failure==NativeChatKeyVault::Failure::None},{"requestedKeyReadEmpty",key.value.bytes.isEmpty()}}},
                {"capture",QJsonObject{{"locale",locale},{"mode",mode},{"phase","pre-click-expanded"},{"terminalPhase","post-canonical-terminal"},
                    {"viewport",QJsonObject{{"width",window->width()},{"height",window->height()},{"dpr",window->devicePixelRatio()}}},
                    {"frame",publicPreStem+".png"},{"metrics",publicPreStem+".json"},{"terminalFrame",publicTerminalStem+".png"},{"terminalMetrics",publicTerminalStem+".json"}}},
                {"transport",QJsonObject{{"snapshots",QJsonObject{{"catalog",snapshot("/api/workspace/v1/messenger/streams/")},{"detail",snapshot("/api/workspace/v1/messenger/streams/"+streamId)},
                    {"topics",snapshot("/api/workspace/v1/messenger/stream_topics/")},{"bindings",snapshot("/api/workspace/v1/messenger/stream_bindings/")},
                    {"users",snapshot("/api/workspace/v1/users/")},{"history",snapshot("history")}}},{"observations",publicObservations},{"messagePost",actualPublicPost},{"canonicalGet",actualCanonicalGet},
                    {"exactMessagePostCount",f.posts},{"canonicalGetCount",exactRequests(f,"GET","/api/workspace/v1/messenger/messages/"+publicId)}}},
                {"workerProjection",QJsonValue::Null},{"sourceRecipientCompletion",completion},
                {"expected",QJsonObject{{"actor",publicPresentation.value("name")},{"title",QJsonObject{{"en",QString("requested a security key for device “%1”").arg(device)},{"ru",QString("запросил(а) ключ безопасности для устройства «%1»").arg(device)}}},
                    {"description",QJsonObject{{"en","This device does not have the requested key"},{"ru","На этом устройстве нет запрошенного ключа"}}},
                    {"rejectLabel",QJsonObject{{"en","Reject"},{"ru","Отклонить"}}},{"terminalStatus",QJsonObject{{"en","Request rejected."},{"ru","Запрос отклонён."}}},
                    {"visibleIds",visibleIds},{"postCount",1},{"canonicalGetCount",1}}}};
            const auto directory=qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");
            QFile output(QDir(directory).filePath("native-"+publicScenario+".public-inputs.json"));QVERIFY(output.open(QIODevice::WriteOnly));
            const auto bytes=QJsonDocument(pack).toJson(QJsonDocument::Indented);QCOMPARE(output.write(bytes),qint64(bytes.size()));
        }
        // Real second input at the old action location cannot write a second response.
        QTest::mouseClick(window, Qt::LeftButton, {}, previousActionPoint);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts, posts + 1);
        QCOMPARE(f.controller.draft(), oldDraft);
        const bool closed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }

    void uiIncomingKeyRequestUsesActualNativeResponseButtons_data() {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("grant"); QTest::addColumn<int>("viewportWidth");
        QTest::newRow("en-light-grant") << QString("en") << QString("light") << true << 1440;
        QTest::newRow("en-light-reject") << QString("en") << QString("light") << false << 1440;
        QTest::newRow("ru-dark-grant") << QString("ru") << QString("dark") << true << 1440;
        QTest::newRow("ru-dark-reject") << QString("ru") << QString("dark") << false << 1440;
        QTest::newRow("ru-dark-grant-narrow") << QString("ru") << QString("dark") << true << 700;
        QTest::newRow("en-light-reject-narrow") << QString("en") << QString("light") << false << 700;
    }
    void uiIncomingKeyRequestUsesActualNativeResponseButtons() {
        QFETCH(QString,locale); QFETCH(QString,mode); QFETCH(bool,grant); QFETCH(int,viewportWidth);
        Fixture f; f.stream=fullEncryptedStream(); f.stream["role"]="administrator";
        const auto roles=addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f,true));
        QCOMPARE(roles->value(userId),QString("administrator"));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY(incomingKeyRequestRow(f.controller,peer.requestId).value(grant?"canGrant":"canReject").toBool());
        const auto row=incomingKeyRequestRow(f.controller,peer.requestId);
        for (const auto *key:{"ownCurrentDevice","canManageKeys","hasRequestedKey","offerGrant","offerReject"}) QVERIFY(row.contains(key));
        QVERIFY(!row.value("ownCurrentDevice").toBool()); QVERIFY(row.value("canManageKeys").toBool());
        QVERIFY(row.value("hasRequestedKey").toBool()); QVERIFY(row.value("offerGrant").toBool()); QVERIFY(row.value("offerReject").toBool());
        for (const auto *key:{"publicKey","privateKey","payload","content","deviceId"}) QVERIFY(!row.contains(key));
        struct Counts { int posts=0,canonicalGets=0; QString expectedPublic; QJsonObject body; };
        const auto counts=std::make_shared<Counts>(); const auto previous=f.http.handler;
        f.http.handler=[previous,counts,requestId=peer.requestId](const Request &request) {
            const auto response=previous(request); const auto path=request.target.path();
            const auto payload=request.body.value("payload").toObject();
            if (request.method=="POST" && path=="/api/workspace/v1/messenger/messages/"
                && payload.value("request_message_uuid").toString()==requestId) {
                ++counts->posts; counts->body=request.body;
                counts->expectedPublic=QUuid::createUuidV5(QUuid(request.body.value("topic_uuid").toString()),request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
            }
            if (request.method=="GET" && !counts->expectedPublic.isEmpty()
                && path=="/api/workspace/v1/messenger/messages/"+counts->expectedPublic) ++counts->canonicalGets;
            return response;
        };
        auto restoreHandler=qScopeGuard([&f,previous]{f.http.handler=previous;});
        NativePreferences preferences(f.directory.filePath("incoming-response-ui.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences);
        QCOMPARE(preferences.locale(),locale); QCOMPARE(preferences.mode(),mode);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow=qScopeGuard([window=QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Incoming request window did not close after inactive focus",__FILE__,__LINE__);
        });
        if (viewportWidth != 1440) window->resize(viewportWidth,900);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_COMPARE(window->size(),QSize(viewportWidth,900));
        QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->size(),QSize(viewportWidth,900)); QCOMPARE(window->devicePixelRatio(),qreal(1));
        QCOMPARE(window->rendererInterface()->graphicsApi(),QSGRendererInterface::OpenGL);
        expandInitialServiceGroup(window,peer.requestId); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyRequestService-"+peer.requestId); QVERIFY(service);
        const auto device=peer.requestDto.value("payload").toObject().value("device_name").toString().trimmed(); QVERIFY(!device.isEmpty());
        const auto title=(locale=="ru"?QString("запросил(а) ключ безопасности для устройства «%1»"):QString("requested a security key for device “%1”")).arg(device);
        auto *titleItem=encryptedUiText(service,title); QVERIFY(titleItem);
        QCOMPARE(titleItem->property("textFormat").toInt(),int(Qt::PlainText));
        if (viewportWidth == 700) QVERIFY(titleItem->property("lineCount").toInt()>1);
        QVERIFY(encryptedUiText(service,row.value("name").toString()));
        auto *grantButton=encryptedUiItem(window,"grantChatKeyButton-"+peer.requestId); QVERIFY(grantButton);
        auto *rejectButton=encryptedUiItem(window,"rejectChatKeyButton-"+peer.requestId); QVERIFY(rejectButton);
        QCOMPARE(grantButton->property("text").toString(),locale=="ru"?QString("Передать ключ"):QString("Send key"));
        QCOMPARE(rejectButton->property("text").toString(),locale=="ru"?QString("Отклонить"):QString("Reject"));
        for (auto *button:{grantButton,rejectButton}) {
            QVERIFY(button->isEnabled()); QCOMPARE(button->height(),qreal(20));
            QCOMPARE(button->property("horizontalPadding").toReal(),qreal(0)); QCOMPARE(button->property("verticalPadding").toReal(),qreal(0));
            const auto font=QQmlProperty(button,"font").read().value<QFont>();
            QCOMPARE(font.pixelSize(),14); QCOMPARE(font.weight(),QFont::Normal); QVERIFY(font.underline());
            auto *content=button->property("contentItem").value<QQuickItem *>(); QVERIFY(content);
            QCOMPARE(content->property("textFormat").toInt(),int(Qt::PlainText));
        }
        if (grant) captureEncryptedUi(window, locale+'-'+mode, "incoming-request", peer.requestId);
        const auto before=f.posts; const auto oldDraft=f.controller.draft();
        auto *action=grant?grantButton:rejectButton; QSignalSpy clicked(action,SIGNAL(clicked())); QVERIFY(clicked.isValid());
        encryptedUiClick(action); QTRY_COMPARE(clicked.size(),1);
        QTRY_COMPARE(counts->posts,1); QTRY_COMPARE(counts->canonicalGets,1);
        QTRY_COMPARE(incomingKeyRequestRow(f.controller,peer.requestId).value("status").toString(),grant?QString("granted"):QString("rejected"));
        QCOMPARE(f.posts,before+1); QCOMPARE(f.controller.draft(),oldDraft);
        const auto body=counts->body,payload=body.value("payload").toObject();
        QCOMPARE(body.value("stream_uuid").toString(),streamId); QCOMPARE(body.value("topic_uuid").toString(),secondTopicId);
        QVERIFY(peer.requestDto.value("topic_uuid").toString()!=body.value("topic_uuid").toString());
        QCOMPARE(f.savedRequest.value("uuid").toString(),counts->expectedPublic);
        QCOMPARE(payload.value("kind").toString(),grant?QString("key_grant"):QString("key_reject"));
        QCOMPARE(payload.value("key_uuid").toString(),returnedId); QCOMPARE(payload.value("request_message_uuid").toString(),peer.requestId);
        QCOMPARE(payload.value("sender_device_id").toString(),f.vault.device().id);
        QCOMPARE(payload.value("recipient_device_id").toString(),SourceRecipientDevice);
        QCOMPARE(payload.contains("content"),grant); if (grant) { QVERIFY(payload.value("content").isString()); QVERIFY(!payload.value("content").toString().contains(peer.expectedText)); }
        const auto result=peer.finish(f.savedRequest,body.value("uuid").toString()); QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("completionOutcome").toString(),grant?QString("granted"):QString("rejected"));
        QCOMPARE(result.value("completionRequest").toString(),peer.requestId); QCOMPARE(result.value("recipientPending").toInt(),0);
        QCOMPARE(result.value("recipientHasKey").toBool(),grant); QCOMPARE(result.value("matchesExpected").toBool(),grant);
        if (grant) QVERIFY(result.value("groupMatchesIssuer").toBool());
        const auto terminalText=grant?(locale=="ru"?QString("Ключ отправлен"):QString("Key sent"))
            :(locale=="ru"?QString("Запрос отклонён."):QString("Request rejected."));
        const auto terminalReady=[&] {auto *holder=encryptedUiItem(window,"keyRequestService-"+peer.requestId);return holder && encryptedUiText(holder,terminalText);};
        QTRY_VERIFY(terminalReady());
        QVERIFY(!encryptedUiItem(window,"grantChatKeyButton-"+peer.requestId)); QVERIFY(!encryptedUiItem(window,"rejectChatKeyButton-"+peer.requestId));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(f.posts,before+1); QCOMPARE(counts->posts,1); QCOMPARE(counts->canonicalGets,1);
        const bool closed=closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }
    void uiIncomingKeyRequestKeepsDisabledActionLabelsWhilePending() {
        Fixture f; f.stream=fullEncryptedStream(); f.stream["role"]="administrator";
        const auto roles=addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        SourceRecipientPeer peer; QVERIFY(peer.prepare(f,true)); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(peer.requestDto)));
        QTRY_VERIFY(incomingKeyRequestRow(f.controller,peer.requestId).value("canGrant").toBool());
        NativePreferences preferences(f.directory.filePath("incoming-pending-ui.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences); QCOMPARE(preferences.locale(),QString("en")); QCOMPARE(preferences.mode(),QString("light"));
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Incoming pending window did not close after inactive focus",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        expandInitialServiceGroup(window,peer.requestId); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyRequestService-"+peer.requestId); QVERIFY(service);
        auto *grant=encryptedUiItem(window,"grantChatKeyButton-"+peer.requestId); QVERIFY(grant); QVERIFY(grant->isEnabled());
        auto *reject=encryptedUiItem(window,"rejectChatKeyButton-"+peer.requestId); QVERIFY(reject); QVERIFY(reject->isEnabled());
        const auto before=f.posts; f.postDelay=1200; // Existing controlled HTTP response delay, not a UI sleep or a latency assertion.
        QSignalSpy clicked(grant,SIGNAL(clicked())); QVERIFY(clicked.isValid()); encryptedUiClick(grant); QTRY_COMPARE(clicked.size(),1);
        QTRY_COMPARE(f.posts,before+1); QTRY_VERIFY(f.controller.encryption().value("keyResponse").toMap().value("busy").toBool());
        const auto row=incomingKeyRequestRow(f.controller,peer.requestId);
        QVERIFY(row.contains("offerGrant") && row.contains("offerReject")); QVERIFY(row.value("offerGrant").toBool()); QVERIFY(row.value("offerReject").toBool());
        QVERIFY(!row.value("canGrant").toBool()); QVERIFY(!row.value("canReject").toBool());
        auto *pendingGrant=encryptedUiItem(window,"grantChatKeyButton-"+peer.requestId); QVERIFY(pendingGrant); QVERIFY(!pendingGrant->isEnabled());
        auto *pendingReject=encryptedUiItem(window,"rejectChatKeyButton-"+peer.requestId); QVERIFY(pendingReject); QVERIFY(!pendingReject->isEnabled());
        QCOMPARE(pendingGrant->property("text").toString(),QString("Send key")); QCOMPARE(pendingReject->property("text").toString(),QString("Reject"));
        QTRY_COMPARE(incomingKeyRequestRow(f.controller,peer.requestId).value("status").toString(),QString("granted"));
        const auto result=peer.finish(f.savedRequest,f.requestBody.value("uuid").toString()); QVERIFY(result.value("ok").toBool());
        QVERIFY(result.value("recipientHasKey").toBool()); QVERIFY(result.value("matchesExpected").toBool()); QCOMPARE(f.posts,before+1);
        const bool closed=closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }
    void uiOwnCurrentDeviceRequestHasNoInlineIssuerActions() {
        Fixture f; f.stream=fullEncryptedStream(); f.stream["role"]="administrator"; f.stream["default_topic_uuid"]=topicId;
        const auto roles=addSourceRecipientBinding(f); useCompleteSendEcho(f); QVERIFY(f.start());
        f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts,1); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        const auto own=f.savedRequest; const auto id=own.value("uuid").toString(); QVERIFY(!id.isEmpty());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        QTRY_VERIFY(incomingKeyRequestRow(f.controller,id).value("canReject").toBool());
        const auto row=incomingKeyRequestRow(f.controller,id);
        for (const auto *key:{"ownCurrentDevice","canManageKeys","hasRequestedKey","offerGrant","offerReject"}) QVERIFY(row.contains(key));
        QCOMPARE(row.value("status").toString(),QString("pending")); QVERIFY(row.value("ownCurrentDevice").toBool());
        QVERIFY(row.value("canManageKeys").toBool()); QVERIFY(!row.value("hasRequestedKey").toBool()); QVERIFY(row.value("offerReject").toBool());
        // Core still permits this exact device response; only Source INLINE actions are absent.
        QVERIFY(row.value("canReject").toBool());
        NativePreferences preferences(f.directory.filePath("incoming-own-device-ui.ini")); preferences.setOwner(f.controller.ownerKey()); preferences.setLocale("ru"); preferences.setMode("dark");
        QQmlApplicationEngine engine; loadNativeUi(engine,&f.controller,&preferences); QCOMPARE(preferences.locale(),QString("ru")); QCOMPARE(preferences.mode(),QString("dark"));
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow=qScopeGuard([window=QPointer<QQuickWindow>(window)] {if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Own request window did not close after inactive focus",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window)); QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        expandInitialServiceGroup(window,id); QVERIFY(!QTest::currentTestFailed());
        auto *service=encryptedUiItem(window,"keyRequestService-"+id); QVERIFY(service);
        const auto device=own.value("payload").toObject().value("device_name").toString().trimmed(); QVERIFY(!device.isEmpty());
        auto *title=encryptedUiText(service,QString("Вы запросили ключ безопасности для устройства «%1»").arg(device)); QVERIFY(title);
        QCOMPARE(title->property("textFormat").toInt(),int(Qt::PlainText));
        QVERIFY(encryptedUiText(service,QString("Подтвердите запрос на другом устройстве с ключом или дождитесь предоставления доступа")));
        QVERIFY(!encryptedUiItem(window,"grantChatKeyButton-"+id)); QVERIFY(!encryptedUiItem(window,"rejectChatKeyButton-"+id));
        QVERIFY(incomingKeyRequestRow(f.controller,id).value("canReject").toBool()); QCOMPARE(f.posts,1);
        const bool closed=closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }
    void uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data() {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        for(const auto *locale:{"en","ru"}) for(const auto *mode:{"light","dark"})
            QTest::newRow(qPrintable(QString(locale)+'-'+mode))<<QString(locale)<<QString(mode);
    }
    void uiEncryptedReadUsesActualSourceGrantAndSafeCopy() {
        QFETCH(QString,locale); QFETCH(QString,mode);
        const auto previousClipboard=QGuiApplication::clipboard()->text();
        const auto restoreClipboard=qScopeGuard([&]{QGuiApplication::clipboard()->setText(previousClipboard);});
        Fixture f; f.stream=fullEncryptedStream(); f.stream["current_encryption_key_uuid"]=OtherGeneration;
        QVERIFY(f.start());
        QTRY_COMPARE(displayed(f.controller,messageId).value("encryptionState").toString(),QString("missing"));
        NativePreferences preferences(f.directory.filePath("ui.ini"));QVERIFY(!f.controller.ownerKey().isEmpty());
        preferences.setOwner(f.controller.ownerKey());preferences.setLocale(locale);preferences.setMode(mode);
        QQmlApplicationEngine engine;loadNativeUi(engine,&f.controller,&preferences);
        QCOMPARE(preferences.locale(),locale);QCOMPARE(preferences.mode(),mode);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(encryptedUiItem(window,"encryptedConversationHeader"));
        auto *placeholder=encryptedUiItem(window,"encrypted-placeholder-"+messageId);QVERIFY(placeholder);QCOMPARE(placeholder->height(),qreal(36));
        QVERIFY(!encryptedUiItem(window,"message-content-"+messageId));
        QVERIFY(!encryptedUiItem(window,"messageComposer"));QCOMPARE(f.posts,0);
        captureEncryptedUi(window,locale+'-'+mode,"missing");
        encryptedUiMenu(window);auto *request=encryptedUiItem(window,"requestMessageKeyAction");QVERIFY(request);
        QTRY_VERIFY(request->isEnabled() && request->height()==36);encryptedUiClick(request);
        QTRY_COMPARE(f.posts,1);QCOMPARE(f.requestBody.value("payload").toObject().value("key_uuid").toString(),returnedId);
        QTRY_COMPARE(f.controller.encryption().value("state").toString(),QString("requested"));
        captureEncryptedUi(window,locale+'-'+mode,"requested");
        const auto source=f.sourceResponse();QVERIFY(source.value("ok").toBool());
        f.cipher["payload"]=source.value("encrypted");QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QTRY_VERIFY(displayed(f.controller,messageId).value("decrypted").toBool());
        QTRY_VERIFY(encryptedUiItem(window,"message-content-"+messageId));
        QVERIFY(!encryptedUiItem(window,"encrypted-placeholder-"+messageId));
        // Readable historical A is not proof that the catalog's current B is
        // available; the real current-generation missing lookup remains true.
        QCOMPARE(f.controller.encryption().value("currentKeyUuid").toString(),OtherGeneration);
        QVERIFY(f.controller.encryption().value("canRequestCurrentKey").toBool());
        auto *footer=encryptedUiItem(window,"encryptedReadStatusText");QVERIFY(footer);
        QCOMPARE(footer->property("text").toString(),locale=="ru"
            ? QString("Ключ безопасности обновился. Запросите ключ у владельца канала для просмотра новых сообщений")
            : QString("The security key has changed. Request the key from the channel owner to read new messages"));
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(),returnedId);
        QVERIFY(f.controller.property("canCompose").toBool());
        QVERIFY(encryptedUiItem(window,"messageComposer"));
        QVERIFY(encryptedUiItem(window,"messageComposer")->isEnabled());
        QVERIFY(!encryptedUiItem(window,"encryptedUnsupportedSendingNotice"));
        QVERIFY(encryptedUiItem(window,"requestCurrentChatKeyButton")->isEnabled());
        captureEncryptedUi(window,locale+'-'+mode,"readable");
        // Renderer-neutral Copy: the visible secret label must not reach the
        // clipboard simply because its enclosing message decrypted correctly.
        const QString markdown="**Visible plain text** [SYNTHETIC PRIVATE LABEL](urn:secret:synthetic-envelope)";
        auto key=f.vault.read(f.vault.lease(),{streamId,returnedId});
        const auto wipe=qScopeGuard([&]{key.value.bytes.fill(0);});QCOMPARE(key.failure,NativeChatKeyVault::Failure::None);
        auto encoded=NativeChatCrypto::encryptMessage(key.value.bytes,markdown.toUtf8(),{streamId,returnedId});QVERIFY(encoded);
        auto payload=f.cipher.value("payload").toObject();payload["content"]=QString::fromLatin1(encoded.bytes.toBase64());f.cipher["payload"]=payload;
        auto change=cryptoEvent(f.cipher);auto body=change.value("payload").toObject();body["kind"]="message.updated";change["payload"]=body;
        QVERIFY(f.controller.applyRealtimeEvent(change));QTRY_COMPARE(displayed(f.controller,messageId).value("content").toString(),markdown);
        NativeMarkdown renderer;const auto plain=renderer.render(markdown).value("plain").toString();
        QVERIFY(!plain.contains("SYNTHETIC PRIVATE LABEL"));QVERIFY(!plain.contains("urn:secret:"));QVERIFY(!plain.contains("**"));
        encryptedUiMenu(window);auto *copy=encryptedUiItem(window,"copyMessageAction");QVERIFY(copy);
        QVERIFY(!encryptedUiItem(window,"replyMessageAction"));QVERIFY(!encryptedUiItem(window,"editMessageAction"));
        QSignalSpy copyTriggered(copy,SIGNAL(triggered()));QVERIFY(copyTriggered.isValid());
        awaitEncryptedCopyLayout(window,copy);
        QGuiApplication::clipboard()->setText("synthetic previous clipboard");encryptedUiClick(copy);
        QTRY_COMPARE(copyTriggered.size(),1);
        QCOMPARE(QGuiApplication::clipboard()->text(),plain);
        QAccessible::setActive(true);auto *accessible=QAccessible::queryAccessibleInterface(encryptedUiItem(window,"message-"+messageId));QVERIFY(accessible);
        QVERIFY(!accessible->text(QAccessible::Name).contains("SYNTHETIC PRIVATE LABEL"));
        QVERIFY(!f.controller.canSend());QVERIFY(!f.controller.canAttach());QCOMPARE(f.posts,1);
        f.session.logout();QTRY_VERIFY(!encryptedUiItem(window,"encryptedReadStatus"));QVERIFY(f.controller.messages().isEmpty());window->close();
    }
    void uiEncryptedSourceKeyNoticePaint_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedSourceKeyNoticePaint() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start()); QVERIFY(f.prepareA());
        QTRY_VERIFY(f.controller.encryption().value("canRequestCurrentKey").toBool());
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
        QCOMPARE(f.controller.encryption().value("currentKeyUuid").toString(), OtherGeneration);
        NativePreferences preferences(f.directory.filePath("source-key-notice-paint.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Notice window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *notice = encryptedUiItem(window, "encryptedReadStatus"); QVERIFY(notice);
        auto *label = encryptedUiItem(window, "encryptedReadStatusText"); QVERIFY(label);
        auto *request = encryptedUiItem(window, "requestCurrentChatKeyButton"); QVERIFY(request); QVERIFY(request->isEnabled());
        auto *dismiss = encryptedUiItem(window, "dismissEncryptedKeyNoticeButton"); QVERIFY(dismiss);
        QVERIFY(window->property("encryptedKeyNoticeVisible").toBool());
        QTest::mouseMove(window, QPoint(0, 0));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->size(), QSize(1440, 900)); QCOMPARE(window->devicePixelRatio(), qreal(1));
        QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::OpenGL);
        const QImage frame = window->grabWindow(); QVERIFY(!frame.isNull());
        const QPoint surfacePoint = notice->mapToScene(QPointF(8, notice->height() - 4)).toPoint();
        const QPoint buttonPoint = request->mapToScene(QPointF(8, request->height() / 2)).toPoint();
        QVERIFY(frame.rect().contains(surfacePoint)); QVERIFY(frame.rect().contains(buttonPoint));
        const QColor surface = frame.pixelColor(surfacePoint), button = frame.pixelColor(buttonPoint);
        const QColor expectedSurface(mode == "dark" ? "#222328" : "#ffffff"), accent("#7087ff");
        const QColor labelColor = label->property("color").value<QColor>();
        auto *content = request->property("contentItem").value<QQuickItem *>(); QVERIFY(content);
        const QColor foreground = content->property("color").value<QColor>();
        const QColor dismissColor = QQmlProperty(dismiss,"icon.color").read().value<QColor>();
        const auto rgb=[](const QColor &c){return QJsonArray{c.red(),c.green(),c.blue(),c.alpha()};};
        qInfo().noquote() << "SOURCE_KEY_NOTICE_PAINT" << QJsonDocument(QJsonObject{
            {"surface",rgb(surface)},{"expectedSurface",rgb(expectedSurface)},{"button",rgb(button)},
            {"accent",rgb(accent)},{"label",rgb(labelColor)},{"foreground",rgb(foreground)},{"dismiss",rgb(dismissColor)},
            {"surfacePoint",QJsonArray{surfacePoint.x(),surfacePoint.y()}},
            {"buttonPoint",QJsonArray{buttonPoint.x(),buttonPoint.y()}},{"graphicsApi",3}
        }).toJson(QJsonDocument::Compact);
        // Flat interior samples avoid text/corner antialiasing; glyph pixels are not compared across engines.
        const auto near=[](const QColor &a,const QColor &b){return qAbs(a.red()-b.red())<=2&&qAbs(a.green()-b.green())<=2&&qAbs(a.blue()-b.blue())<=2&&a.alpha()==b.alpha();};
        QVERIFY2(near(surface, expectedSurface), "Source key notice shares the opaque composer surface");
        QVERIFY2(near(button, accent), "Source key notice Request is filled primary");
        QCOMPARE(labelColor, accent); QCOMPARE(dismissColor, accent);
        QCOMPARE(foreground, QColor(mode == "dark" ? "#141517" : "#1b1b1d"));
        QCOMPARE(f.posts, 1); QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        const bool closed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }
    void uiEncryptedSourceKeyNoticeGeometry_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedSourceKeyNoticeGeometry() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start()); QVERIFY(f.prepareA());
        QTRY_VERIFY(f.controller.encryption().value("canRequestCurrentKey").toBool());
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
        QCOMPARE(f.controller.encryption().value("currentKeyUuid").toString(), OtherGeneration);
        NativePreferences preferences(f.directory.filePath("source-key-notice-geometry.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data())) QTest::qFail("Notice window did not close after inactive focus", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *notice = encryptedUiItem(window, "encryptedReadStatus"); QVERIFY(notice);
        auto *label = encryptedUiItem(window, "encryptedReadStatusText"); QVERIFY(label);
        auto *request = encryptedUiItem(window, "requestCurrentChatKeyButton"); QVERIFY(request);
        auto *dismiss = encryptedUiItem(window, "dismissEncryptedKeyNoticeButton"); QVERIFY(dismiss);
        auto *composer = encryptedUiItem(window, "messageComposerShell"); QVERIFY(composer);
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->size(), QSize(1440, 900)); QCOMPARE(window->devicePixelRatio(), qreal(1));
        const auto local=[notice](QQuickItem *part){return part->mapRectToItem(notice,QRectF(0,0,part->width(),part->height()));};
        const QRectF labelRect=local(label),requestRect=local(request),dismissRect=local(dismiss);
        const auto rect=[](const QRectF &r){return QJsonArray{r.x(),r.y(),r.width(),r.height()};};
        qInfo().noquote() << "SOURCE_KEY_NOTICE_GEOMETRY" << QJsonDocument(QJsonObject{
            {"notice",QJsonArray{notice->width(),notice->height()}},{"label",rect(labelRect)},
            {"request",rect(requestRect)},{"dismiss",rect(dismissRect)},
            {"noticeBottom",notice->mapToScene(QPointF(0,notice->height())).y()},
            {"composerTop",composer->mapToScene(QPointF()).y()}
        }).toJson(QJsonDocument::Compact);
        const QString expected=locale=="ru"?QString("Ключ безопасности обновился. Запросите ключ у владельца канала для просмотра новых сообщений")
            :QString("The security key has changed. Request the key from the channel owner to read new messages");
        QCOMPARE(label->property("text").toString(), expected);
        QCOMPARE(labelRect.left(), qreal(12)); QCOMPARE(requestRect.top(), qreal(12));
        QCOMPARE(label->height(), qreal(20)); QCOMPARE(request->height(), qreal(32));
        QVERIFY(qAbs(requestRect.left()-labelRect.right()-20)<=1);
        QVERIFY(qAbs(labelRect.center().y()-requestRect.center().y())<=1);
        QCOMPARE(dismiss->size(), QSizeF(24,24));
        QVERIFY(qAbs(dismissRect.left()-requestRect.right()-8)<=1);
        QVERIFY(qAbs(dismissRect.center().y()-requestRect.center().y())<=1);
        QCOMPARE(request->property("horizontalPadding").toReal(), qreal(12));
        QCOMPARE(QQmlProperty(request,"font.pixelSize").read().toInt(), 12);
        QCOMPARE(QQmlProperty(request,"font.weight").read().toInt(), int(QFont::Normal));
        QVERIFY(qAbs(notice->height()-(qMax(label->height(),request->height())+12))<=1);
        QVERIFY(qAbs(notice->mapToScene(QPointF(0,notice->height())).y()-composer->mapToScene(QPointF()).y())<=1);
        QCOMPARE(f.posts, 1); QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        const bool closed = closeEncryptedVisualWindow(window); closeWindow.dismiss(); QVERIFY(closed);
    }
    void uiEncryptedSourceHeaderAvatarUsesDefaultThemeCircle_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedSourceHeaderAvatarUsesDefaultThemeCircle() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start());
        QTRY_COMPARE(displayed(f.controller, messageId).value("encryptionState").toString(), QString("missing"));
        NativePreferences preferences(f.directory.filePath("source-avatar.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data()))
                QTest::qFail("Visual window teardown did not observe inactive focus before close", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *header = encryptedUiItem(window, "encryptedConversationHeader"); QVERIFY(header);
        auto *face = encryptedUiAvatarCircle(header, 32); QVERIFY(face);
        QCOMPARE(face->size(), QSizeF(32, 32)); QCOMPARE(face->property("radius").toReal(), qreal(16));
        QQuickItem *lock = nullptr;
        for (auto *child : face->childItems())
            if (child->isVisible() && child->property("asset").toString() == "chat_lock.svg") { lock = child; break; }
        QVERIFY(lock); QCOMPARE(lock->property("glyphSize").toInt(), 20);
        QCOMPARE(QQmlProperty(lock, "icon.width").read().toInt(), 20);
        QCOMPARE(QQmlProperty(lock, "icon.height").read().toInt(), 20);
        // Source Avatar sm: bg-bg + 1px border-subtle. Null color uses these
        // exact pinned blueCold tokens, not the surrounding header surface.
        QCOMPARE(face->property("color").value<QColor>(), QColor(mode == "dark" ? "#141517" : "#e4ecf3"));
        QCOMPARE(QQmlProperty(face, "border.width").read().toReal(), qreal(1));
        QCOMPARE(QQmlProperty(face, "border.color").read().value<QColor>(), QColor(mode == "dark" ? "#33384d" : "#d8e4ef"));
        QCOMPARE(f.posts, 0);
        const bool closed = closeEncryptedVisualWindow(window); closeWindow.dismiss();
        QVERIFY2(closed, "Visual window teardown did not observe inactive focus before close");
    }
    void uiEncryptedSourceSidebarAvatarUsesLockFallback_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedSourceSidebarAvatarUsesLockFallback() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start());
        QTRY_COMPARE(displayed(f.controller, messageId).value("encryptionState").toString(), QString("missing"));
        const auto streams = f.controller.streams();
        QVERIFY(!streams.isEmpty()); QVERIFY(streams.first().toMap().value("encrypted").toBool());
        NativePreferences preferences(f.directory.filePath("source-avatar.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data()))
                QTest::qFail("Visual window teardown did not observe inactive focus before close", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        auto *row = encryptedUiItem(window, "stream-" + streamId); QVERIFY(row);
        auto *face = encryptedUiAvatarCircle(row, 40); QVERIFY(face);
        QCOMPARE(face->size(), QSizeF(40, 40)); QCOMPARE(face->property("radius").toReal(), qreal(20));
        QCOMPARE(face->property("color").value<QColor>(), QColor(mode == "dark" ? "#141517" : "#e4ecf3"));
        QCOMPARE(QQmlProperty(face, "border.color").read().value<QColor>(), QColor(mode == "dark" ? "#33384d" : "#d8e4ef"));
        QQuickItem *lock = nullptr;
        for (auto *child : face->childItems())
            if (child->isVisible() && child->property("asset").toString() == "chat_lock.svg") { lock = child; break; }
        // The actual encrypted fallback must be a lock, not a letter painted
        // inside an otherwise correct circle; no avatar URL or loader is added.
        QVERIFY2(lock, "Encrypted sidebar avatar must render the source chat_lock.svg fallback");
        QCOMPARE(lock->property("glyphSize").toInt(), 24);
        QCOMPARE(QQmlProperty(lock, "icon.width").read().toInt(), 24);
        QCOMPARE(QQmlProperty(lock, "icon.height").read().toInt(), 24);
        QCOMPARE(encryptedUiTextCount(face, "E"), 0);
        QCOMPARE(f.posts, 0);
        const bool closed = closeEncryptedVisualWindow(window); closeWindow.dismiss();
        QVERIFY2(closed, "Visual window teardown did not observe inactive focus before close");
    }
    void uiEncryptedSourceHeaderGeometry_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedSourceHeaderGeometry() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start());
        QTRY_COMPARE(displayed(f.controller, messageId).value("encryptionState").toString(), QString("missing"));
        NativePreferences preferences(f.directory.filePath("source-visual.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        // Close once on normal completion or an assertion return, before engine teardown.
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data()))
                QTest::qFail("Visual window teardown did not observe inactive focus before close", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->width(), 1440); QCOMPARE(window->height(), 900);
        auto *shell = encryptedUiItem(window, "conversationHeader"); QVERIFY(shell);
        auto *header = encryptedUiItem(window, "encryptedConversationHeader"); QVERIFY(header);
        auto *title = encryptedUiItem(window, "encryptedConversationTitle"); QVERIFY(title);
        auto *stream = encryptedUiText(header, "Encrypted fixture"); QVERIFY(stream);
        auto *protectedInfo = encryptedUiItem(window, "protectedChatInformation"); QVERIFY(protectedInfo);
        const auto titleRect = encryptedUiRect(title, shell), streamRect = encryptedUiRect(stream, shell);
        const auto protectedRect = encryptedUiRect(protectedInfo, shell);
        qInfo().noquote() << "SOURCE_HEADER" << QJsonDocument(QJsonObject{{"shell", encryptedUiRect(shell, shell)},
            {"header", encryptedUiRect(header, shell)}, {"title", titleRect}, {"stream", streamRect},
            {"protected", protectedRect}}).toJson(QJsonDocument::Compact);
        // Actual source DOM: pt12 + title20 + mt4 + subtitle18 + pb8 = 62.
        // Source's member/presence text stays unknown; never invent zero counts.
        QCOMPARE(shell->height(), qreal(62));
        QCOMPARE(header->height(), qreal(42));
        QCOMPARE(titleRect.value("x").toDouble(), qreal(64));
        QCOMPARE(titleRect.value("y").toDouble(), qreal(12));
        QCOMPARE(title->property("textFormat").toInt(), 0); // Text.PlainText
        QVERIFY(qAbs(streamRect.value("x").toDouble() - titleRect.value("x").toDouble() - title->width() - 8) <= 1);
        QVERIFY(qAbs(protectedRect.value("x").toDouble() - streamRect.value("x").toDouble() - stream->width() - 20) <= 1);
        auto *subtitle = encryptedUiItem(window, "encryptedParticipantsPlaceholder"); QVERIFY(subtitle);
        QCOMPARE(subtitle->height(), qreal(18));
        QCOMPARE(subtitle->mapToItem(shell, QPointF{}).y(), qreal(36));
        QVERIFY(encryptedUiTextCount(subtitle, "0 members, 0 online") == 0);
        // Long metadata and a narrow real window exercise truncation, not a
        // second style fixture. Plain text must stay inside the packed group.
        f.stream["name"] = QString("<b>synthetic</b> ") + QString(200, QChar('W'));
        auto changed = cryptoEvent(f.stream); auto payload = changed.value("payload").toObject();
        payload["kind"] = "stream.updated"; changed["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(changed));
        window->resize(700, 600); QTRY_COMPARE(window->width(), 700);
        QSignalSpy narrowFrame(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!narrowFrame.isEmpty());
        const auto narrowTitle = title->mapToScene(QPointF{}), narrowInfo = protectedInfo->mapToScene(QPointF{});
        QVERIFY(title->width() > 0 && title->width() < title->property("implicitWidth").toReal());
        QVERIFY(narrowTitle.x() + title->width() <= narrowInfo.x() - 20 + 1);
        QVERIFY(protectedInfo->mapToScene(QPointF(protectedInfo->width(), 0)).x() <= shell->mapToScene(QPointF(shell->width() - 20, 0)).x());
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach()); QCOMPARE(f.posts, 0);
        const bool closed = closeEncryptedVisualWindow(window);
        closeWindow.dismiss();
        QVERIFY2(closed, "Visual window teardown did not observe inactive focus before close");
    }
    void uiEncryptedSourceLockedGeometry_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedSourceLockedGeometry() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start());
        QTRY_COMPARE(displayed(f.controller, messageId).value("encryptionState").toString(), QString("missing"));
        NativePreferences preferences(f.directory.filePath("source-visual.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        // Close once on normal completion or an assertion return, before engine teardown.
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data()))
                QTest::qFail("Visual window teardown did not observe inactive focus before close", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->width(), 1440); QCOMPARE(window->height(), 900);
        auto *bubble = encryptedUiItem(window, "message-" + messageId); QVERIFY(bubble);
        auto *placeholder = encryptedUiItem(window, "encrypted-placeholder-" + messageId); QVERIFY(placeholder);
        auto *time = encryptedUiItem(window, "message-time-" + messageId); QVERIFY(time);
        auto *author = encryptedUiText(bubble->parentItem(), Issuer); QVERIFY(author);
        qInfo().noquote() << "SOURCE_LOCKED_GEOMETRY" << QJsonDocument(QJsonObject{
            {"bubble", encryptedUiRect(bubble, bubble)}, {"placeholder", encryptedUiRect(placeholder, bubble)},
            {"time", encryptedUiRect(time, bubble)}, {"author", encryptedUiRect(author, bubble->parentItem())}}).toJson(QJsonDocument::Compact);
        QCOMPARE(placeholder->size(), QSizeF(480, 36));
        QCOMPARE(bubble->size(), QSizeF(504, 52));
        QCOMPARE(placeholder->mapToItem(bubble, QPointF{}), QPointF(12, 8));
        QCOMPARE(bubble->width() - time->mapToItem(bubble, QPointF{}).x() - time->width(), qreal(12));
        QCOMPARE(bubble->height() - time->mapToItem(bubble, QPointF{}).y() - time->height(), qreal(8));
        QCOMPARE(time->height(), qreal(16));
        QCOMPARE(bubble->mapToScene(QPointF{}).y() - author->mapToScene(QPointF{}).y(), qreal(24));
        QVERIFY(!encryptedUiItem(window, "message-content-" + messageId));
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach()); QCOMPARE(f.posts, 0);
        const bool closed = closeEncryptedVisualWindow(window);
        closeWindow.dismiss();
        QVERIFY2(closed, "Visual window teardown did not observe inactive focus before close");
    }
    void uiEncryptedSourceLockedPaint_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedSourceLockedPaint() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start());
        QTRY_COMPARE(displayed(f.controller, messageId).value("encryptionState").toString(), QString("missing"));
        NativePreferences preferences(f.directory.filePath("source-visual.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        // Close once on normal completion or an assertion return, before engine teardown.
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data()))
                QTest::qFail("Visual window teardown did not observe inactive focus before close", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->width(), 1440); QCOMPARE(window->height(), 900);
        QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::OpenGL);
        QCOMPARE(window->devicePixelRatio(), qreal(1));
        auto *placeholder = encryptedUiItem(window, "encrypted-placeholder-" + messageId); QVERIFY(placeholder);
        QCOMPARE(placeholder->size(), QSizeF(480, 36));
        const auto origin = placeholder->mapToScene(QPointF{});
        const QImage pixels = window->grabWindow(); QVERIFY(!pixels.isNull());
        // Actual source DOM h36; first bar y6/h8/w92%, second y22/h8/w65%.
        // At x75% only the first bar contributes. This tests broad spread and
        // relative contrast with raster tolerance, never exact Chrome/Qt pixels.
        const int x = qRound(origin.x() + placeholder->width() * .75), y = qRound(origin.y());
        const int background = qRed(pixels.pixel(x, y + 35));
        QJsonArray samples; QVector<int> profile; int peak = 0;
        for (int i = 0; i < 36; ++i) {
            const int value = mode == "dark" ? qRed(pixels.pixel(x, y + i)) - background
                                             : background - qRed(pixels.pixel(x, y + i));
            profile.append(value); samples.append(value); peak = qMax(peak, value);
        }
        QVERIFY(peak > 0); int first = -1, last = -1;
        for (int i = 0; i < profile.size(); ++i) if (profile[i] * 2 >= peak) { if (first < 0) first = i; last = i; }
        const int halfPeakSpan = last - first + 1;
        const int unblurredContrast = mode == "dark" ? qRound((112 - background) * .35) : qRound((background - 152) * .35);
        QVERIFY(unblurredContrast > 0);
        const qreal relativePeak = qreal(peak) / unblurredContrast;
        qInfo().noquote() << "SOURCE_LOCKED_PAINT" << QJsonDocument(QJsonObject{{"samples", samples},
            {"background", background}, {"peak", peak}, {"halfPeakSpan", halfPeakSpan}, {"relativePeak", relativePeak},
            {"graphicsApi", int(window->rendererInterface()->graphicsApi())}}).toJson(QJsonDocument::Compact);
        // Preserved source Chrome157: EN-light peak18/span16, RU-dark12/span17.
        // Their unblurred contrasts are36/25. Allow broad raster variation.
        QVERIFY2(halfPeakSpan >= 12 && halfPeakSpan <= 22, "Locked bars need the source's broad Gaussian spread, not the current 8px hard band");
        QVERIFY2(relativePeak >= .30 && relativePeak <= .70, "Blurred contrast should remain near half the unchanged unblurred bar contrast");
        QCOMPARE(f.posts, 0); const bool closed = closeEncryptedVisualWindow(window);
        closeWindow.dismiss();
        QVERIFY2(closed, "Visual window teardown did not observe inactive focus before close");
    }
    void uiEncryptedReadableFooterHasOneAvailabilityNotice_data() { uiEncryptedReadUsesActualSourceGrantAndSafeCopy_data(); }
    void uiEncryptedReadableFooterHasOneAvailabilityNotice() {
        QFETCH(QString, locale); QFETCH(QString, mode);
        Fixture f; f.stream = fullEncryptedStream(); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        QVERIFY(f.start());
        QTRY_COMPARE(displayed(f.controller, messageId).value("encryptionState").toString(), QString("missing"));
        NativePreferences preferences(f.directory.filePath("source-visual.ini"));
        preferences.setOwner(f.controller.ownerKey()); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &f.controller, &preferences);
        QCOMPARE(preferences.locale(), locale); QCOMPARE(preferences.mode(), mode);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        // Close once on normal completion or an assertion return, before engine teardown.
        auto closeWindow = qScopeGuard([window = QPointer<QQuickWindow>(window)] {
            if (!closeEncryptedVisualWindow(window.data()))
                QTest::qFail("Visual window teardown did not observe inactive focus before close", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(window->width(), 1440); QCOMPARE(window->height(), 900);
        // Real source-JS grant is fixture setup; the regression concerns the
        // displayed footer, not a synthetic ready flag or another input path.
        f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_COMPARE(f.controller.encryption().value("state").toString(), QString("requested"));
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QTRY_VERIFY(encryptedUiItem(window, "message-content-" + messageId));
        QCOMPARE(f.controller.encryption().value("currentKeyUuid").toString(), OtherGeneration);
        QVERIFY(f.controller.encryption().value("canRequestCurrentKey").toBool());
        auto *footer = encryptedUiItem(window, "encryptedReadStatus"); QVERIFY(footer);
        const QString unavailable = locale == "ru" ? QString("Отправка сообщений в защищённых чатах пока недоступна.")
                                                   : QString("Sending encrypted messages is currently unavailable");
        const QString ready = locale == "ru" ? QString("Ключ доступен на этом устройстве") : QString("The key is available on this device");
        QSignalSpy footerFrame(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!footerFrame.isEmpty());
        qInfo() << "SOURCE_FOOTER_COUNT" << encryptedUiTextCount(footer, unavailable);
        QCOMPARE(encryptedUiTextCount(footer, unavailable), 0);
        QCOMPARE(encryptedUiTextCount(footer, ready), 0);
        const QString updated = locale == "ru"
            ? QString("Ключ безопасности обновился. Запросите ключ у владельца канала для просмотра новых сообщений")
            : QString("The security key has changed. Request the key from the channel owner to read new messages");
        QCOMPARE(encryptedUiTextCount(footer, updated), 1);
        QCOMPARE(f.controller.encryption().value("sendingKeyUuid").toString(), returnedId);
        QVERIFY(f.controller.property("canCompose").toBool());
        QVERIFY(encryptedUiItem(window, "messageComposer"));
        QVERIFY(encryptedUiItem(window, "messageComposer")->isEnabled());
        QVERIFY(encryptedUiItem(window, "requestCurrentChatKeyButton")->isEnabled());
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach()); QCOMPARE(f.posts, 1);
        const bool closed = closeEncryptedVisualWindow(window);
        closeWindow.dismiss();
        QVERIFY2(closed, "Visual window teardown did not observe inactive focus before close");
    }
    void uiEncryptedMissingErrorArchiveAndNullCurrentRemainDistinct_data() {
        QTest::addColumn<QString>("state");
        for(const auto *state:{"archived","nullable","provider","known-error"})QTest::newRow(state)<<QString(state);
    }
    void uiEncryptedMissingErrorArchiveAndNullCurrentRemainDistinct() {
        QFETCH(QString,state);Fixture f;f.stream=fullEncryptedStream(state=="archived");
        if(state=="nullable")f.stream["current_encryption_key_uuid"]=QJsonValue::Null;
        if(state=="provider"){f.stream["source_name"]="zulip";f.stream["provider_uuid"]=Issuer;f.stream["provider_identity"]=IssuerDevice;}
        QVERIFY(f.start(state=="archived"));
        if(state=="known-error") {
            auto key=NativeChatCrypto::createGroupKey();QVERIFY(key);const auto wipe=qScopeGuard([&]{key.bytes.fill(0);});
            QCOMPARE(f.vault.put(f.vault.lease(),{streamId,returnedId},key.bytes,0),NativeChatKeyVault::Failure::None);
        }
        QTRY_COMPARE(displayed(f.controller,messageId).value("encryptionState").toString(),state=="known-error"?QString("error"):QString("missing"));
        NativePreferences preferences(f.directory.filePath("ui.ini"));preferences.setLocale("en");
        QQmlApplicationEngine engine;loadNativeUi(engine,&f.controller,&preferences);auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        if(state=="known-error") {
            // A known current key can compose even when this history cipher is malformed.
            QTRY_COMPARE(f.controller.encryption().value("sendingKeyUuid").toString(),returnedId);
            QVERIFY(f.controller.property("canCompose").toBool());
            QTRY_VERIFY(encryptedUiItem(window,"messageComposer"));
            QVERIFY(encryptedUiItem(window,"messageComposer")->isEnabled());
            QVERIFY(window->property("encryptedComposerHasKey").toBool());
            QVERIFY(!encryptedUiItem(window,"encryptedReadStatus"));
            QVERIFY(encryptedUiItem(window,"encrypted-error-"+messageId));
        } else QTRY_VERIFY(encryptedUiItem(window,"encryptedReadStatus"));
        if(state=="provider" || state=="known-error") {
            encryptedUiMenu(window);
            auto *reactionBar=encryptedUiItem(window,"reactionMenuBar");QVERIFY(reactionBar);
            auto *menuWindow=reactionBar->window();QVERIFY(menuWindow);
            QSignalSpy rendered(menuWindow,&QQuickWindow::frameSwapped);menuWindow->update();QTRY_VERIFY(!rendered.isEmpty());
            QVERIFY(reactionBar->isVisible());
            QVERIFY(!encryptedUiItem(window,"requestMessageKeyAction"));QVERIFY(!encryptedUiItem(window,"requestCurrentChatKeyButton"));
            QCOMPARE(f.posts,0);
        }
        else {
            encryptedUiMenu(window);auto *request=encryptedUiItem(window,"requestMessageKeyAction");QVERIFY(request);QVERIFY(request->isEnabled());
            if(state=="nullable")QVERIFY(!encryptedUiItem(window,"requestCurrentChatKeyButton"));
            encryptedUiClick(request);QTRY_COMPARE(f.posts,1);
            QCOMPARE(f.requestBody.value("payload").toObject().value("key_uuid").toString(),returnedId);
        }
        if(state=="archived"){QVERIFY(f.controller.selectedTopic().isEmpty());for(const auto &req:f.http.requests)QVERIFY(!req.target.path().contains("unarchive"));}
        QVERIFY(!f.controller.canSend());QVERIFY(!f.controller.canAttach());window->close();
    }
    void uiEncryptedOsRetryDoesNotSendAKeyRequest() {
        // Sentinels outlive Fixture, including any assertion-return teardown.
        bool alive=true;int expiredCallbacks=0;
        Fixture f;f.stream=fullEncryptedStream();QVERIFY(f.start());
        bindNativeChatKeys(f.vault,f.session,f.api,f.controller,"Linux — Desktop");QTRY_VERIFY(f.controller.encryption().value("available").toBool());
        NativePreferences preferences(f.directory.filePath("ui.ini"));QQmlApplicationEngine engine;loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        f.controller.setChatKeyStoreOpening(true);QTRY_VERIFY(!encryptedUiItem(window,"retryChatKeyStoreButton"));
        f.controller.setChatKeyStoreOpening(false);f.vault.close();QTRY_VERIFY(encryptedUiItem(window,"retryChatKeyStoreButton"));
        {
            QSignalSpy retry(&f.controller,&NativeController::chatKeyStoreOpenRequested);bool observedOpening=false;
            // This guard only closes the sentinel scope; it never disconnects.
            const auto markExpired=qScopeGuard([&]{alive=false;});
            const auto openingConnection=connect(&f.controller,&NativeController::stateChanged,this,[&]{
                if(!alive){++expiredCallbacks;return;}
                observedOpening|=f.controller.encryption().value("storeOpening").toBool();
            });
            const auto disconnectOpening=qScopeGuard([openingConnection]{QObject::disconnect(openingConnection);});
            auto *button=encryptedUiItem(window,"retryChatKeyStoreButton");button->forceActiveFocus(Qt::TabFocusReason);QTest::keyClick(button->window(),Qt::Key_Space);
            QTRY_COMPARE(retry.size(),1);QTRY_VERIFY(f.controller.encryption().value("available").toBool());QVERIFY(observedOpening);
            QVERIFY(!encryptedUiItem(window,"retryChatKeyStoreButton"));QCOMPARE(f.posts,0);QCOMPARE(f.streamGets,0);QCOMPARE(f.bindingGets,0);
        }
        // No event pumping: close drives the real synchronous store observer.
        alive=false;
        f.vault.close();
        QCOMPARE(expiredCallbacks,0);
        window->close();
    }
    void uiEncryptedCheckIsGetOnlyAndRetryKeepsTheSameIntent_data() {
        QTest::addColumn<bool>("applied");QTest::newRow("applied")<<true;QTest::newRow("unapplied")<<false;
    }
    void uiEncryptedCheckIsGetOnlyAndRetryKeepsTheSameIntent() {
        QFETCH(bool,applied);Fixture f;f.stream=fullEncryptedStream();QVERIFY(f.start());
        const auto normal=f.http.handler;f.http.handler=[&,normal](const Request &request) {
            auto response=normal(request);
            if(request.method=="POST" && request.target.path()=="/api/workspace/v1/messenger/messages/" && f.posts==1) {
                if(!applied)f.savedRequests.clear();response.status=503;response.data=QJsonObject{};
            }
            return response;
        };
        NativePreferences preferences(f.directory.filePath("ui.ini"));QQmlApplicationEngine engine;loadNativeUi(engine,&f.controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(encryptedUiItem(window,"requestCurrentChatKeyButton"));encryptedUiClick(encryptedUiItem(window,"requestCurrentChatKeyButton"));
        QTRY_COMPARE(f.posts,1);QTRY_COMPARE(f.controller.encryption().value("state").toString(),QString("unresolved"));const auto first=f.requestBody;
        QTRY_VERIFY(encryptedUiItem(window,"checkChatKeyRequestButton"));
        auto *check=encryptedUiItem(window,"checkChatKeyRequestButton");
        QSignalSpy checkPressed(check,SIGNAL(pressed())),checkReleased(check,SIGNAL(released())),checkClicked(check,SIGNAL(clicked()));
        using InputCounts=std::array<int,3>;
        const auto counts=[](const QSignalSpy &pressed,const QSignalSpy &released,const QSignalSpy &clicked) {
            return InputCounts{int(pressed.size()),int(released.size()),int(clicked.size())};
        };
        InputCounts checkBefore{-1,-1,-1},checkAfterClick{-1,-1,-1},checkAfterNotBusy{-1,-1,-1};
        InputCounts retryBefore{-1,-1,-1},retryAfterClick{-1,-1,-1},retryAfterPosts{-1,-1,-1};
        const char *inputStage="check-spies";bool inputCompleted=false;
        const auto inputFailure=qScopeGuard([&,check=QPointer<QQuickItem>(check),window=QPointer<QQuickWindow>(window)] {
            if(inputCompleted)return;
            const auto array=[](const InputCounts &c){return QJsonArray{c[0],c[1],c[2]};};
            const auto encryption=f.controller.encryption();
            qInfo().noquote()<<"ENCRYPTED_CHECK_RETRY_INPUT"<<QJsonDocument(QJsonObject{
                {"stage",QString::fromLatin1(inputStage)},{"applied",applied},{"posts",f.posts},
                {"checkBefore",array(checkBefore)},{"checkAfterClick",array(checkAfterClick)},
                {"checkAfterNotBusy",array(checkAfterNotBusy)},{"retryBefore",array(retryBefore)},
                {"retryAfterClick",array(retryAfterClick)},{"retryAfterPosts",array(retryAfterPosts)},
                {"state",encryption.value("state").toString()},{"busy",encryption.value("busy").toBool()},
                {"checkAlive",bool(check)},{"checkVisible",check&&check->isVisible()},
                {"checkEnabled",check&&check->isEnabled()},{"windowAlive",bool(window)}
            }).toJson(QJsonDocument::Compact);
        });
        struct ActionLayout {
            QRectF scene,clip; bool visible=false,enabled=false,sameWindow=false;
            bool ready() const { return visible&&enabled&&sameWindow&&scene.width()>0&&scene.height()>0&&clip.contains(scene); }
        };
        const auto layout=[window](QQuickItem *action) {
            ActionLayout value;
            if(!action)return value;
            value.scene=action->mapRectToScene(QRectF(0,0,action->width(),action->height()));
            value.clip=QRectF(0,0,window->width(),window->height());
            for(auto *parent=action->parentItem();parent;parent=parent->parentItem())
                if(parent->clip())value.clip=value.clip.intersected(parent->mapRectToScene(QRectF(0,0,parent->width(),parent->height())));
            value.visible=action->isVisible();value.enabled=action->isEnabled();value.sameWindow=action->window()==window;
            return value;
        };
        ActionLayout checkPreFrame,checkPostFrame,retryPreFrame,retryPostFrame;
        bool checkFrameObserved=false,retryFrameObserved=false;
        const auto layoutReport=qScopeGuard([&] {
            const auto rect=[](const QRectF &r){return QJsonArray{r.x(),r.y(),r.width(),r.height()};};
            const auto value=[&](const ActionLayout &v){return QJsonObject{{"scene",rect(v.scene)},{"clip",rect(v.clip)},
                {"visible",v.visible},{"enabled",v.enabled},{"sameWindow",v.sameWindow},{"ready",v.ready()}};};
            qInfo().noquote()<<"ENCRYPTED_CHECK_RETRY_LAYOUT"<<QJsonDocument(QJsonObject{
                {"applied",applied},{"checkFrameObserved",checkFrameObserved},{"retryFrameObserved",retryFrameObserved},
                {"checkPreFrame",value(checkPreFrame)},{"checkPostFrame",value(checkPostFrame)},
                {"retryPreFrame",value(retryPreFrame)},{"retryPostFrame",value(retryPostFrame)}
            }).toJson(QJsonDocument::Compact);
        });
        QVERIFY(checkPressed.isValid());QVERIFY(checkReleased.isValid());QVERIFY(checkClicked.isValid());
        inputStage="check-frame";checkPreFrame=layout(check);
        QSignalSpy checkFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!checkFrame.isEmpty());checkFrameObserved=true;
        inputStage="check-hit-ready";QTRY_VERIFY((checkPostFrame=layout(check)).ready());
        checkBefore=counts(checkPressed,checkReleased,checkClicked);inputStage="check-click";
        encryptedUiClick(check);checkAfterClick=counts(checkPressed,checkReleased,checkClicked);
        inputStage="check-click-receipt";QTRY_COMPARE(checkClicked.size(),1);
        inputStage="check-not-busy";
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());QCOMPARE(f.posts,1);
        checkAfterNotBusy=counts(checkPressed,checkReleased,checkClicked);inputStage="check-outcome";
        if(applied)QCOMPARE(f.controller.encryption().value("state").toString(),QString("requested"));
        else {
            QCOMPARE(f.controller.encryption().value("state").toString(),QString("unresolved"));auto *retry=encryptedUiItem(window,"retryChatKeyRequestButton");QVERIFY(retry);
            QSignalSpy retryPressed(retry,SIGNAL(pressed())),retryReleased(retry,SIGNAL(released())),retryClicked(retry,SIGNAL(clicked()));
            QVERIFY(retryPressed.isValid());QVERIFY(retryReleased.isValid());QVERIFY(retryClicked.isValid());
            inputStage="retry-frame";retryPreFrame=layout(retry);
            QSignalSpy retryFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!retryFrame.isEmpty());retryFrameObserved=true;
            inputStage="retry-hit-ready";QTRY_VERIFY((retryPostFrame=layout(retry)).ready());
            retryBefore=counts(retryPressed,retryReleased,retryClicked);inputStage="retry-click";
            encryptedUiClick(retry);retryAfterClick=counts(retryPressed,retryReleased,retryClicked);
            inputStage="retry-click-receipt";QTRY_COMPARE(retryClicked.size(),1);
            inputStage="retry-posts";
            QTRY_COMPARE(f.posts,2);retryAfterPosts=counts(retryPressed,retryReleased,retryClicked);
            inputStage="retry-outcome";
            QTRY_COMPARE(f.controller.encryption().value("state").toString(),QString("requested"));QCOMPARE(f.requestBody,first);
        }
        inputStage="final-gates";
        QVERIFY(!f.controller.canSend());QVERIFY(!f.controller.canAttach());window->close();inputCompleted=true;
    }
#endif

    void publicPreparationStateDoesNotOfferAKeyRequestForKnownMalformedCiphertext_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("missing") << QString("missing");
        QTest::newRow("known-malformed") << QString("error");
        QTest::newRow("readable") << QString("ready");
    }
    void publicPreparationStateDoesNotOfferAKeyRequestForKnownMalformedCiphertext() {
        QFETCH(QString, kind);
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        if (kind == "ready") QVERIFY(f.prepareA());
        else if (kind == "error") {
            auto key = NativeChatCrypto::createGroupKey(); QVERIFY(key);
            const auto wipe = qScopeGuard([&] { key.bytes.fill(0); });
            QCOMPARE(f.vault.put(f.vault.lease(), {streamId, returnedId}, key.bytes, 0), NativeChatKeyVault::Failure::None);
        }
        QTRY_COMPARE_WITH_TIMEOUT(displayed(f.controller, messageId).value("encryptionState").toString(), kind, 2000);
        QCOMPARE(displayed(f.controller, messageId).value("canRequestKey").toBool(), kind == "missing");
        QCOMPARE(f.controller.encryption().value("canRequestCurrentKey").toBool(), kind == "missing");
        QCOMPARE(displayed(f.controller, messageId).value("decrypted").toBool(), kind == "ready");
        const auto expectedPosts = kind == "ready" ? 1 : 0;
        QCOMPARE(f.posts, expectedPosts);
        if (kind != "ready") {
            const auto publicJson = QJsonDocument(QJsonObject::fromVariantMap(displayed(f.controller, messageId))).toJson();
            QVERIFY(!publicJson.contains("Synthetic source-granted plaintext"));
            QCOMPARE(f.streamGets, 0); QCOMPARE(f.bindingGets, 0);
        }
    }

    void publicEncryptionMetadataKeepsHistoricalCurrentAndRequestGenerationsDistinct_data() {
        QTest::addColumn<bool>("nullable"); QTest::addColumn<bool>("provider");
        QTest::newRow("different-current") << false << false;
        QTest::newRow("no-current-historical-request") << true << false;
        QTest::newRow("provider-cannot-request") << false << true;
    }
    void publicEncryptionMetadataKeepsHistoricalCurrentAndRequestGenerationsDistinct() {
        QFETCH(bool, nullable); QFETCH(bool, provider);
        Fixture f; f.stream = fullEncryptedStream();
        f.stream["current_encryption_key_uuid"] = nullable ? QJsonValue(QJsonValue::Null) : QJsonValue(OtherGeneration);
        if (provider) {
            f.stream["source_name"] = "zulip"; f.stream["provider_uuid"] = Issuer;
            f.stream["provider_identity"] = IssuerDevice;
        }
        QVERIFY(f.start());
        const auto state = f.controller.encryption();
        QVERIFY(state.value("available").toBool());
        QCOMPARE(state.value("canRequest").toBool(), !provider);
        QCOMPARE(state.value("currentKeyUuid").toString(), nullable || provider ? QString{} : OtherGeneration);
        QVERIFY(state.value("keyUuid").toString().isEmpty()); // Last request, not the current generation.
        const auto row = displayed(f.controller, messageId);
        QCOMPARE(row.value("encryptionKeyUuid").toString(), returnedId);
        QVERIFY(!row.value("decrypted").toBool());
        const auto publicJson = QJsonDocument(QJsonObject::fromVariantMap(row)).toJson();
        QVERIFY(!publicJson.contains("encoded_content")); QVERIFY(!publicJson.contains("private_key"));
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 0); QCOMPARE(f.bindingGets, 0);
        f.controller.selectStream({});
        QVERIFY(f.controller.encryption().value("currentKeyUuid").toString().isEmpty());
        QVERIFY(!f.controller.encryption().value("canRequest").toBool());
        QVERIFY(f.controller.encryption().value("available").toBool());
    }


    // These new intersection cases are deliberately unrun in the unfixed clone.
    // Old archive-only crypto gates remain so root can observe genuine REDs.
    void archivedHistoricalCiphertextReadsTheAlreadyAvailableExactGeneration() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start()); QVERIFY(f.prepareA());
        const auto generation = returnedId;
        const auto posts = f.posts, details = f.streamGets, bindings = f.bindingGets;
        f.stream = fullEncryptedStream(true); f.stream["current_encryption_key_uuid"] = OtherGeneration;
        f.controller.selectStream({}); f.controller.refresh();
        QTRY_COMPARE(f.controller.archivedChats().size(), 1);
        QVERIFY(f.controller.streams().isEmpty());
        f.controller.openArchivedChat(streamId);
        QTRY_COMPARE(f.controller.selectedStream(), streamId); QVERIFY(f.controller.selectedTopic().isEmpty());
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Synthetic source-granted plaintext"));
        const auto raw = parseMessage(f.cipher, projectId); QVERIFY(raw && raw->crypto);
        QCOMPARE(raw->crypto->keyUuid, generation); QVERIFY(raw->content.isEmpty());
        QCOMPARE(f.posts, posts); QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings);
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        for (const auto &request : f.http.requests)
            QVERIFY(!request.target.path().contains("unarchive"));
    }

    void archivedMissingGenerationCanRequestAndReceiveAnActualSourceGrant() {
        Fixture f; f.stream = fullEncryptedStream(true); QVERIFY(f.start(true));
        QVERIFY(f.controller.selectedTopic().isEmpty());
        f.controller.requestChatKey(returnedId);
        QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.requestBody.value("topic_uuid").toString(), secondTopicId);
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        f.cipher["payload"] = source.value("encrypted");
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        const auto publicId = f.savedRequest.value("uuid").toString();
        const auto receipt = f.vault.requestCompletionByMessage(f.vault.lease(), {streamId, returnedId}, publicId);
        QVERIFY(receipt.value); QCOMPARE(receipt.value->outcome, NativeChatKeyVault::Outcome::Granted);
        QVERIFY(f.controller.selectedTopic().isEmpty());
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        for (const auto &request : f.http.requests) QVERIFY(!request.target.path().contains("unarchive"));
    }

    void pureArchiveMetadataDoesNotBrieflyRevokePreparedPlaintext() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start()); QVERIFY(f.prepareA());
        const auto posts = f.posts, details = f.streamGets, bindings = f.bindingGets;
        bool hidden = false;
        connect(&f.controller, &NativeController::stateChanged, this, [&] {
            if (f.controller.selectedStream() == streamId)
                hidden |= !displayed(f.controller, messageId).value("decrypted").toBool();
        });
        f.stream = fullEncryptedStream(true);
        auto event = cryptoEvent(f.stream); auto payload = event.value("payload").toObject();
        payload["kind"] = "stream.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event));
        QVERIFY(!hidden);
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.controller.archivedChats().size(), 1); QCOMPARE(f.controller.selectedTopic(), topicId);
        QCOMPARE(f.posts, posts); QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings);
        auto retained = f.vault.read(f.vault.lease(), {streamId, returnedId});
        const auto scrub = qScopeGuard([&] { retained.value.bytes.fill(0); });
        QCOMPARE(retained.failure, NativeChatKeyVault::Failure::None); QCOMPARE(retained.value.bytes.size(), 32);
        QCOMPARE(retained.value.deletionRevision, quint64(0));
    }

    void correlatedRequestMovingOutDuringAuthorityCannotAuthorizeTheOldGrant() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", true, pending, body, own));
        const auto scrub = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        const QString otherStream = "33333333-3333-4333-8333-333333333334";
        const QString otherTopic = "55555555-5555-4555-8555-555555555556";
        auto other = fullEncryptedStream(); other["uuid"] = otherStream;
        other["default_topic_uuid"] = otherTopic; other["name"] = "Other owned encrypted channel";
        f.controller.selectStream({});
        const auto normal = f.http.handler;
        f.http.handler = [&, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/streams/")
                return Response{QJsonArray{f.stream, other}};
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{QJsonArray{own, f.cipher}};
            return normal(request);
        };
        f.controller.refresh(); QTRY_COMPARE(f.controller.streams().size(), 2);
        const auto detailBefore = f.streamGets, bindingBefore = f.bindingGets;
        f.streamDelay = 1200; // Existing fixture delay; individual QTRY budgets stay unchanged.
        f.controller.selectStream(streamId); QTRY_COMPARE(f.controller.messages().size(), 2);
        QTRY_COMPARE(f.streamGets, detailBefore + 1);
        bool detailInFlight = false;
        for (const auto reply : f.api.findChildren<QNetworkReply *>())
            detailInFlight |= reply->url().path() == "/api/workspace/v1/messenger/streams/" + streamId && !reply->isFinished();
        QVERIFY2(detailInFlight, "Fixture precondition: original shared authority detail reply must still be pending");
        QVERIFY(!incomingKeyRequestRow(f.controller, own.value("uuid").toString()).isEmpty());
        QCOMPARE(f.bindingGets, bindingBefore);
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        auto moved = own; moved["stream_uuid"] = otherStream; moved["topic_uuid"] = otherTopic;
        auto event = cryptoEvent(moved); auto payload = event.value("payload").toObject();
        payload["kind"] = "message.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event));
        QVERIFY(displayed(f.controller, own.value("uuid").toString()).isEmpty());
        QTRY_COMPARE(f.bindingGets, bindingBefore + 1);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        auto key = f.vault.read(f.vault.lease(), {streamId, returnedId});
        const auto wipeKey = qScopeGuard([&] { key.value.bytes.fill(0); });
        QVERIFY(key.value.bytes.isEmpty()); QCOMPARE(f.posts, 0);
        QCOMPARE(f.streamGets, detailBefore + 1);
    }

    void threeFreshProcessesRecoverPendingReceiptAndKeyThroughActualSourceGrant() {
        Fixture f;
        QTemporaryDir persisted(QDir::tempPath() + "/cassi-process-vault-XXXXXX"); QVERIFY(persisted.isValid());
        auto master = NativeChatCrypto::createGroupKey(); QVERIFY(master);
        const auto wipe = qScopeGuard([&] { master.bytes.fill(0); master.bytes.clear(); });
        QFile certificate(CertificatePath); QVERIFY(certificate.open(QIODevice::ReadOnly));
        const auto publicCertificate = certificate.readAll();
        QJsonObject common{{"server", f.tls.origin().toString()}, {"directory", persisted.path()},
            {"certificate", QString::fromLatin1(publicCertificate.toBase64())}};
        QJsonArray stages;
        auto run = [&](const QString &mode) {
            auto publicInput = common; publicInput["mode"] = mode;
            const auto json = QJsonDocument(publicInput).toJson(QJsonDocument::Compact);
            QByteArray wire(4, char(0)); qToBigEndian<quint32>(json.size(), reinterpret_cast<uchar *>(wire.data()));
            wire.append(json); wire.append(master.bytes);
            const auto scrub = qScopeGuard([&] { wire.fill(0); wire.clear(); });
            QProcess child; child.setProgram(QCoreApplication::applicationFilePath());
            child.setArguments({"--process-worker"});
            QByteArray output; qsizetype errorBytes = 0;
            connect(&child, &QProcess::readyReadStandardOutput, this, [&] {
                output.append(child.readAllStandardOutput()); if (output.size() > 4096) child.kill();
            });
            connect(&child, &QProcess::readyReadStandardError, this, [&] {
                auto bytes = child.readAllStandardError(); errorBytes += bytes.size(); bytes.fill(0);
                if (errorBytes > 64 * 1024) child.kill();
            });
            const auto before = f.http.requests.size();
            child.start();
            if (!QTest::qWaitFor([&] { return child.state() == QProcess::Running; }, 2000)) return QJsonObject{};
            const auto pid = child.processId();
            child.write(wire); child.closeWriteChannel();
            const bool exited = QTest::qWaitFor([&] { return child.state() == QProcess::NotRunning; }, 15000);
            if (!exited) { child.kill(); child.waitForFinished(2000); }
            output.append(child.readAllStandardOutput());
            auto errors = child.readAllStandardError(); errorBytes += errors.size(); errors.fill(0);
            auto status = QJsonDocument::fromJson(output).object();
            status["normalExit"] = exited && child.exitStatus() == QProcess::NormalExit;
            status["exit"] = child.exitCode(); status["stderrBytes"] = qint64(errorBytes);
            status["expectedPid"] = pid;
            QJsonObject paths;
            for (int i = before; i < f.http.requests.size(); ++i) {
                const auto &request = f.http.requests.at(i);
                const auto path = QString::fromLatin1(request.method) + ' ' + request.target.path();
                paths[path] = paths.value(path).toInt() + 1;
            }
            status["paths"] = paths; stages.append(status); return status;
        };
        const auto admitted = run("admit");
        QVERIFY2(admitted.value("normalExit").toBool(), qPrintable(admitted.value("phase").toString()));
        QCOMPARE(admitted.value("exit").toInt(), 0); QCOMPARE(admitted.value("stderrBytes").toInt(), 0);
        QCOMPARE(admitted.value("pid"), admitted.value("expectedPid")); QCOMPARE(f.posts, 1);
        QCOMPARE(admitted.value("publicId"), f.savedRequest.value("uuid"));
        QCOMPARE(admitted.value("targetTopic").toString(), secondTopicId);
        const auto localId = admitted.value("localId").toString();
        QCOMPARE(localId, f.requestBody.value("uuid").toString());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        const auto grant = f.responseDto(source);
        f.cipher["payload"] = source.value("encrypted");
        common["localId"] = localId; common["publicId"] = admitted.value("publicId");
        common["device"] = admitted.value("device"); common["responseId"] = grant.value("uuid");
        bool replayControls = true;
        const auto normal = f.http.handler;
        f.http.handler = [&, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{replayControls ? QJsonArray{grant, f.cipher, f.savedRequest} : QJsonArray{f.cipher}};
            return normal(request);
        };
        const int authorityBefore = f.streamGets, bindingsBefore = f.bindingGets;
        const auto completed = run("complete");
        QVERIFY2(completed.value("normalExit").toBool(), qPrintable(completed.value("phase").toString()));
        QCOMPARE(completed.value("exit").toInt(), 0); QCOMPARE(completed.value("stderrBytes").toInt(), 0);
        QVERIFY(completed.value("receipt").toBool()); QVERIFY(completed.value("decrypted").toBool());
        QVERIFY(completed.value("restoredPending").toBool()); QCOMPARE(completed.value("pending").toInt(), 0);
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets - authorityBefore, 1); QCOMPARE(f.bindingGets - bindingsBefore, 1);
        replayControls = false;
        const int restoredAuthority = f.streamGets, restoredBindings = f.bindingGets;
        const auto restored = run("restore");
        QVERIFY2(restored.value("normalExit").toBool(), qPrintable(restored.value("phase").toString()));
        QCOMPARE(restored.value("exit").toInt(), 0); QCOMPARE(restored.value("stderrBytes").toInt(), 0);
        QVERIFY(restored.value("receipt").toBool()); QVERIFY(restored.value("decrypted").toBool());
        QCOMPARE(restored.value("pending").toInt(), 0); QCOMPARE(f.posts, 1);
        QCOMPARE(f.streamGets, restoredAuthority); QCOMPARE(f.bindingGets, restoredBindings);
        QSet<qint64> pids{QCoreApplication::applicationPid()};
        for (const auto &stage : stages) {
            const auto status = stage.toObject(); const auto pid = status.value("pid").toInteger();
            QVERIFY(pid > 0 && !pids.contains(pid)); pids.insert(pid);
            QCOMPARE(status.value("pid"), status.value("expectedPid"));
            QCOMPARE(status.value("owner").toString(), SessionVault::accountId(f.tls.origin(), projectId, userId));
            QCOMPARE(status.value("server").toString(), f.tls.origin().toString());
            QCOMPARE(status.value("device"), admitted.value("device"));
            QCOMPARE(status.value("masterWrites").toInt(), 0); QCOMPARE(status.value("warnings").toInt(), 0);
        }
        QDirIterator files(persisted.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) { QFile file(files.next()); QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll(); QVERIFY(!bytes.contains(master.bytes));
            QVERIFY(!bytes.contains("Synthetic source-granted plaintext")); QVERIFY(!file.fileName().endsWith(".lock")); }
        qInfo().noquote() << "Safe fresh-process phases:" << QJsonDocument(stages).toJson(QJsonDocument::Compact);
    }

    void currentAuthoritativeLoadedRequestMustSupersedeOlderDeferredCatchUp_data() {
        QTest::addColumn<QString>("field"); QTest::addColumn<QString>("localId");
        for (const auto &field : {QString("device_id"), QString("public_key")}) {
            const auto lower = field.toLatin1() + "-below-oldest";
            const auto higher = field.toLatin1() + "-above-oldest";
            QTest::newRow(lower.constData()) << field << QString("10000000-0000-4000-8000-000000000002");
            QTest::newRow(higher.constData()) << field << QString("10000000-0000-4000-8000-000000000001");
        }
    }
    void currentAuthoritativeLoadedRequestMustSupersedeOlderDeferredCatchUp() {
        QFETCH(QString, field); QFETCH(QString, localId); Fixture f;
        // The request must be in the authoritative loaded head, independently
        // of the V5 UUID tie-breaker. Keep both UUID sides as regression rows.
        f.cipher["created_at"] = "2026-01-02T09:59:59.000Z";
        bool fresh = false; QJsonObject changed;
        const auto normal = f.http.handler;
        f.http.handler = [&](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/") {
                if (fresh) return Response{QJsonArray{changed, f.cipher}};
                return Response{QJsonArray{f.cipher}, 200, 0, {{"X-Pagination-Marker", messageId.toLatin1()}}};
            }
            return normal(request);
        };
        QVERIFY(f.start()); f.controller.selectStream(streamId); QTRY_VERIFY(f.controller.canLoadOlder());
        RealtimeTransport transport(f); QVERIFY(transport.start()); QTRY_VERIFY(f.controller.canLoadOlder());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own, localId));
        QVERIFY(own.value("created_at").toString() > f.cipher.value("created_at").toString());
        qInfo().noquote() << "BOUNDARY_WITNESS" << own.value("uuid").toString()
            << "oldest" << messageId << "sameCreated"
            << (own.value("created_at") == f.cipher.value("created_at"))
            << "fitsUuidBoundary" << (own.value("uuid").toString() >= messageId);
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        changed = own; auto payload = changed.value("payload").toObject();
        payload[field] = field == "device_id" ? QString("changed-current-device")
            : QString::fromLatin1(QByteArray(32, 'v').toBase64()); changed["payload"] = payload;
        bool dispatched = false;
        const auto observation = connect(&f.controller, &NativeController::messagesChanged, this, [&] {
            if (!dispatched && f.controller.syncState() != "live"
                && displayed(f.controller, own.value("uuid").toString()).value("id").isValid()) {
                dispatched = true; fresh = true; f.controller.loadOlder();
            }
        });
        const auto disconnectObservation = qScopeGuard([&] { QObject::disconnect(observation); });
        transport.holdReady = true;
        for (auto &socket : transport.sockets) if (socket) socket->close();
        transport.publish(20, "message.created", own, false);
        transport.publish(47, "message.created", f.responseDto(source), false);
        QTRY_VERIFY(dispatched); QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(47));
        QTRY_VERIFY(!transport.pendingReady.isEmpty()); QTRY_VERIFY(!f.controller.busy());
        QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QVERIFY(displayed(f.controller, own.value("uuid").toString()).value("id").isValid());
        QVERIFY(f.controller.syncState() != "live");
        transport.releaseReady(); QTRY_COMPARE(f.controller.syncState(), QString("live"));
        QTRY_COMPARE(f.bindingGets, 1); QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1);
    }

    // Exercise the actual vault-destroyed -> controller messagesChanged path.
    // The sentinel observes an expired capture without dereferencing that capture.
    void requestReplayObserverIsDisconnectedBeforeFixtureTeardown() {
        int activeCallbacks = 0, expiredCallbacks = 0;
        bool observerScopeAlive = true;
        auto fixture = std::make_unique<Fixture>();
        {
            const QJsonObject own = message({}, messageId, secondTopicId);
            const auto markObserverExpired = qScopeGuard([&] { observerScopeAlive = false; });
            const auto observation = connect(&fixture->controller, &NativeController::messagesChanged, this, [&] {
                if (!observerScopeAlive) { ++expiredCallbacks; return; }
                if (own.value("uuid").toString() == messageId) ++activeCallbacks;
            });
            const auto disconnectObservation = qScopeGuard([&] { QObject::disconnect(observation); });
            fixture->controller.useChatKeys(&fixture->vault);
            QCOMPARE(activeCallbacks, 1);
            observerScopeAlive = false;
        }
        fixture.reset();
        QCOMPARE(expiredCallbacks, 0);
    }

    void realWssGrantBeforeDelayedAckUsesRestCursorAndCommitsExactlyOnce() {
        Fixture f; QVERIFY(f.start()); RealtimeTransport transport(f); QVERIFY(transport.start());
        QCOMPARE(transport.snapshots, 1); QCOMPARE(f.tls.origin().scheme(), QString("https"));
        const auto *socket = transport.sockets.last().data(); QVERIFY(socket);
        QCOMPARE(socket->subprotocol(), QString("workspace.events.v1"));
        const auto protocols = socket->request().rawHeader("Sec-WebSocket-Protocol");
        QVERIFY(protocols.contains("workspace.events.v1")); QVERIFY(protocols.contains("bearer.synthetic-project-token"));
        const auto url = socket->requestUrl(); QCOMPARE(url.path(), QString("/api/workspace/v1/events/ws"));
        QCOMPARE(QUrlQuery(url).queryItemValue("epoch_generation"), transport.generation);
        QVERIFY(!url.toString().contains("token")); QVERIFY(!url.toString().contains("bearer"));
        f.postDelay = 1200; f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        auto pending = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        QVERIFY(pending.messageId.isEmpty());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        QSignalSpy changes(&f.vault, &NativeChatKeyVault::keyMaterialChanged);
        transport.duplicateRows = true;
        const auto grant = transport.publish(17, "message.created", f.responseDto(source));
        f.cipher["payload"] = source.value("encrypted");
        transport.publish(29, "message.created", f.cipher);
        transport.send(grant);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(29));
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value.has_value());
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        transport.send(grant); transport.client.nudge();
        QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(changes.size(), 1); QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        QCOMPARE(transport.snapshots, 1); QCOMPARE(transport.client.acknowledgedEpoch(), quint64(29));
    }

    void realWssReconnectCatchesSparseRestHistoryAndCompletesLegacyRequest() {
        Fixture f; QVERIFY(f.start()); RealtimeTransport transport(f); QVERIFY(transport.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "unbound", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        for (auto &socket : transport.sockets) if (socket) socket->close();
        transport.publish(20, "message.created", own, false);
        transport.publish(47, "message.created", f.responseDto(source), false);
        f.cipher["payload"] = source.value("encrypted"); transport.publish(83, "message.created", f.cipher, false);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(83));
        QTRY_VERIFY(transport.sockets.size() >= 2); QTRY_COMPARE(f.controller.syncState(), QString("live"));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QVERIFY(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value.has_value());
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1); QCOMPARE(transport.snapshots, 1);
        QCOMPARE(QUrlQuery(transport.sockets.last()->requestUrl()).queryItemValue("last_epoch_version"), QString("83"));
    }

    void realWssSelfAccessLossBeforeDelayedGrantAuthorityBlocksReceipt() {
        Fixture f; QVERIFY(f.start()); RealtimeTransport transport(f); QVERIFY(transport.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.streamDelay = 300;
        transport.publish(14, "message.created", own);
        transport.publish(19, "message.created", f.responseDto(source));
        QTRY_COMPARE(f.streamGets, 1);
        transport.publish(23, "stream_binding.deleted", selfBindingEvent("stream_binding.deleted").value("payload").toObject());
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(23));
        QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 0); QCOMPARE(f.bindingGets, 0); QCOMPARE(transport.snapshots, 1);
    }

    void realWssReconnectRevocationOrRequestDeletionBeforeDrainCannotCommit_data() {
        QTest::addColumn<bool>("revoked"); QTest::newRow("request-deleted") << false;
        QTest::newRow("self-access-revoked") << true;
    }
    void realWssReconnectRevocationOrRequestDeletionBeforeDrainCannotCommit() {
        QFETCH(bool, revoked); Fixture f; QVERIFY(f.start());
        RealtimeTransport transport(f); QVERIFY(transport.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        for (auto &socket : transport.sockets) if (socket) socket->close();
        transport.publish(20, "message.created", own, false);
        transport.publish(47, "message.created", f.responseDto(source), false);
        if (revoked) transport.publish(83, "stream_binding.deleted",
            selfBindingEvent("stream_binding.deleted").value("payload").toObject(), false);
        else transport.publish(83, "message.deleted", QJsonObject{{"uuid", own.value("uuid")},
            {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}}, false);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(83));
        QTRY_COMPARE(f.controller.syncState(), QString("live"));
        if (!revoked) QTRY_COMPARE(f.bindingGets, 1);
        QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        auto saved = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}); QCOMPARE(saved.values.size(), 1);
        saved.values.first().clearSecrets();
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, revoked ? 0 : 1); QCOMPARE(transport.snapshots, 1);
    }

    void realWssCurrentGenerationNullAndDefaultChangeKeepHistoricalCheckReadOnly() {
        Fixture f; QVERIFY(f.start()); RealtimeTransport transport(f); QVERIFY(transport.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", true, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        f.savedRequests.insert(own.value("uuid").toString(), own);
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.stream["current_encryption_key_uuid"] = QJsonValue::Null;
        f.stream["default_topic_uuid"] = topicId;
        transport.publish(15, "stream.updated", f.stream);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(15));
        f.controller.checkChatKeyRequest(returnedId);
        QTRY_COMPARE(f.controller.encryption().value("state").toString(), QString("requested"));
        auto saved = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        QCOMPARE(saved.targetTopic, secondTopicId); QCOMPARE(saved.localId, pending.localId);
        QCOMPARE(saved.messageId, own.value("uuid").toString()); saved.clearSecrets();
        f.cipher["payload"] = source.value("encrypted");
        transport.publish(31, "message.created", f.responseDto(source));
        transport.publish(58, "message.created", f.cipher);
        QTRY_COMPARE(transport.client.acknowledgedEpoch(), quint64(58));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1); QCOMPARE(transport.snapshots, 1);
    }

    void explicitRetryUsesOneSameCanonicalPostOnlyForTheUnchangedOriginalDefault_data() {
        QTest::addColumn<bool>("changed"); QTest::newRow("same") << false; QTest::newRow("changed") << true;
    }
    void explicitRetryUsesOneSameCanonicalPostOnlyForTheUnchangedOriginalDefault() {
        QFETCH(bool, changed); Fixture f; QVERIFY(f.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "unbound", true, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        if (changed) f.stream["default_topic_uuid"] = topicId;
        f.controller.retryChatKeyRequest(returnedId);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        auto saved = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        const auto scrub = qScopeGuard([&] { saved.clearSecrets(); });
        QCOMPARE(saved.localId, pending.localId); QCOMPARE(saved.targetTopic, pending.targetTopic);
        QCOMPARE(saved.publicKey, pending.publicKey); QCOMPARE(saved.privateKey, pending.privateKey);
        QCOMPARE(saved.deviceName, pending.deviceName);
        if (changed) {
            QCOMPARE(f.posts, 0); QVERIFY(saved.messageId.isEmpty());
            QCOMPARE(f.controller.encryption().value("state").toString(), QString("unresolved"));
        } else {
            QCOMPARE(f.posts, 1); QCOMPARE(f.requestBody, body); QCOMPARE(saved.messageId, own.value("uuid").toString());
            QCOMPARE(f.controller.encryption().value("state").toString(), QString("requested"));
        }
        const auto before = f.posts;
        f.controller.checkChatKeyRequest(returnedId); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.posts, before); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        int canonical = 0;
        for (const auto &request : f.http.requests) if (request.method == "GET"
            && request.target.path() == "/api/workspace/v1/messenger/messages/" + own.value("uuid").toString()) ++canonical;
        QCOMPARE(canonical, 2);
    }

    void reopenedVaultAndFreshControllerRecoverLegacyWithoutPostingOrRetargeting() {
        Fixture f; QVERIFY(f.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "legacy-local", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted"); const auto grant = f.responseDto(source);
        f.controller.useChatKeys(nullptr); f.controller.selectStream({}); f.controller.setRealtimeMode(true);
        const auto previousDevice = f.vault.device().id;
        f.vault.close(); bool opened = false;
        f.vault.open(f.tls.origin(), projectId, userId, "Changed current label",
            [&](NativeChatKeyVault::Failure result) { opened = result == NativeChatKeyVault::Failure::None; });
        QTRY_VERIFY(opened); QCOMPARE(f.vault.device().id, previousDevice);
        const auto normal = f.http.handler;
        f.http.handler = [&, own, grant, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{QJsonArray{grant, f.cipher, own}};
            return normal(request);
        };
        f.streamDelay = 200;
        NativeController restored(&f.api, &f.session); restored.useChatKeys(&f.vault);
        restored.refresh(); QTRY_COMPARE(restored.streams().size(), 1);
        restored.selectStream(streamId); QTRY_COMPARE(restored.messages().size(), 3);
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), address).values.isEmpty()
            && f.vault.pendingRequests(f.vault.lease(), address).values.first().messageId == own.value("uuid").toString());
        auto retained = f.vault.pendingRequests(f.vault.lease(), address).values.first();
        const auto scrub = qScopeGuard([&] { retained.clearSecrets(); });
        QCOMPARE(retained.localId, pending.localId); QCOMPARE(retained.targetTopic, QString{});
        QCOMPARE(retained.deviceName, pending.deviceName); QVERIFY(retained.privateKey == pending.privateKey);
        QVERIFY(retained.publicKey == pending.publicKey);
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
        QTRY_VERIFY(displayed(restored, messageId).value("decrypted").toBool());
        QCOMPARE(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value->requestMessage, own.value("uuid").toString());
        QVERIFY(f.vault.pendingRequests(f.vault.lease(), address).values.isEmpty());
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void completedPublicRequestReplayCannotRebindTheRemainingLegacyPair() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto scrub = qScopeGuard([&] { pending.clearSecrets(); });
        auto other = pending; other.localId = QUuid::createUuid().toString(QUuid::WithoutBraces); other.messageId.clear();
        const auto scrubOther = qScopeGuard([&] { other.clearSecrets(); });
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QCOMPARE(f.vault.putPendingRequest(f.vault.lease(), address, other), NativeChatKeyVault::Failure::None);
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted");
        // Both controls are outside the selected topic: no incidental loaded-R
        // replay can obscure the explicit post-completion full RT update below.
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        auto before = f.vault.pendingRequests(f.vault.lease(), address);
        const auto scrubBefore = qScopeGuard([&] { for (auto &item : before.values) item.clearSecrets(); });
        QCOMPARE(before.failure, NativeChatKeyVault::Failure::None); QCOMPARE(before.values.size(), 1);
        QCOMPARE(before.values.first().localId, other.localId); QVERIFY(before.values.first().messageId.isEmpty());
        const auto receipt = f.vault.requestCompletionByMessage(f.vault.lease(), address, own.value("uuid").toString());
        QCOMPARE(receipt.failure, NativeChatKeyVault::Failure::None); QVERIFY(receipt.value);
        QCOMPARE(receipt.value->localId, pending.localId);
        const auto posts = f.posts, details = f.streamGets, bindings = f.bindingGets;
        QVERIFY(f.controller.applyRealtimeEvent(updatedControl(own)));
        auto after = f.vault.pendingRequests(f.vault.lease(), address);
        const auto scrubAfter = qScopeGuard([&] { for (auto &item : after.values) item.clearSecrets(); });
        QCOMPARE(after.failure, NativeChatKeyVault::Failure::None); QCOMPARE(after.values.size(), 1);
        QCOMPARE(after.values.first().localId, other.localId); QVERIFY(after.values.first().messageId.isEmpty());
        const auto afterReceipt = f.vault.requestCompletionByMessage(f.vault.lease(), address, own.value("uuid").toString());
        QCOMPARE(afterReceipt.failure, NativeChatKeyVault::Failure::None); QVERIFY(afterReceipt.value);
        QCOMPARE(afterReceipt.value->localId, pending.localId);
        QCOMPARE(f.posts, posts); QCOMPARE(f.streamGets, details); QCOMPARE(f.bindingGets, bindings);
    }

    void loadedOwnRequestIsObservedAndBoundBeforeTheSharedAuthorityReply() {
        Fixture f; f.stream = fullEncryptedStream(); QVERIFY(f.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "legacy-local", false, pending, body, own));
        const auto scrub = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted"); const auto grant = f.responseDto(source);
        f.controller.selectStream({});
        const auto normal = f.http.handler;
        f.http.handler = [&, own, grant, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{QJsonArray{grant, own, f.cipher}};
            return normal(request);
        };
        const auto restore = qScopeGuard([&] { f.http.handler = normal; });
        // Existing fixture delay; no individual QTRY budget/timeout changes.
        f.streamDelay = 1200;
        const auto details = f.streamGets, bindings = f.bindingGets;
        f.controller.selectStream(streamId); QTRY_COMPARE(f.controller.messages().size(), 3);
        QTRY_COMPARE(f.streamGets, details + 1);
        bool detailInFlight = false;
        for (const auto reply : f.api.findChildren<QNetworkReply *>())
            detailInFlight |= reply->url().path() == "/api/workspace/v1/messenger/streams/" + streamId && !reply->isFinished();
        QVERIFY2(detailInFlight, "Fixture precondition: shared authority detail reply must still be pending");
        QCOMPARE(f.bindingGets, bindings);
        const NativeChatKeyVault::Address address{streamId, returnedId};
        auto observed = f.vault.pendingRequests(f.vault.lease(), address);
        const auto scrubObserved = qScopeGuard([&] { for (auto &item : observed.values) item.clearSecrets(); });
        QCOMPARE(observed.failure, NativeChatKeyVault::Failure::None); QCOMPARE(observed.values.size(), 1);
        QCOMPARE(observed.values.first().messageId, own.value("uuid").toString());
        QVERIFY(!incomingKeyRequestRow(f.controller, own.value("uuid").toString()).isEmpty());
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, details + 1); QCOMPARE(f.bindingGets, bindings + 1);
    }

    void loadedForeignRequestOnlyStillHydratesOneSharedIssuerAuthority() {
        Fixture f; f.stream = fullEncryptedStream(); f.stream["role"] = "administrator";
        addSourceRecipientBinding(f); QVERIFY(f.start()); SourceRecipientPeer peer;
        QVERIFY(peer.prepare(f, false));
        f.controller.selectStream({}); const auto normal = f.http.handler;
        f.http.handler = [&, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{QJsonArray{peer.requestDto, f.cipher}};
            return normal(request);
        };
        const auto restore = qScopeGuard([&] { f.http.handler = normal; });
        const auto details = f.streamGets, bindings = f.bindingGets;
        f.controller.selectStream(streamId); QTRY_COMPARE(f.controller.messages().size(), 2);
        QTRY_VERIFY(incomingKeyRequestRow(f.controller, peer.requestId).value("canReject").toBool());
        QVERIFY(!incomingKeyRequestRow(f.controller, peer.requestId).value("canGrant").toBool());
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, details + 1); QCOMPARE(f.bindingGets, bindings + 1);
        QVERIFY(peer.finish().value("recipientStillPending").toBool());
    }

    void ambiguousLegacyPairDoesNotBindWhileExactPublicBindingTakesPriority_data() {
        QTest::addColumn<bool>("bound"); QTest::newRow("ambiguous") << false; QTest::newRow("bound-priority") << true;
    }
    void ambiguousLegacyPairDoesNotBindWhileExactPublicBindingTakesPriority() {
        QFETCH(bool, bound);
        Fixture f; QVERIFY(f.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, bound ? "public" : "unbound", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        auto other = pending; other.localId = QUuid::createUuid().toString(QUuid::WithoutBraces); other.messageId.clear();
        const auto scrub = qScopeGuard([&] { other.clearSecrets(); });
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QCOMPARE(f.vault.putPendingRequest(f.vault.lease(), address, other), NativeChatKeyVault::Failure::None);
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted"); const auto grant = f.responseDto(source);
        const auto normal = f.http.handler;
        f.http.handler = [&, own, grant, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{QJsonArray{grant, own, f.cipher}};
            return normal(request);
        };
        f.controller.selectStream(streamId); QTRY_COMPARE(f.controller.messages().size(), 3);
        QTRY_COMPARE(f.bindingGets, 1); QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QCoreApplication::processEvents();
        if (bound) {
            QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
            QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
            auto remaining = f.vault.pendingRequests(f.vault.lease(), address); QCOMPARE(remaining.values.size(), 1);
            QCOMPARE(remaining.values.first().localId, other.localId); QVERIFY(remaining.values.first().messageId.isEmpty());
            for (auto &value : remaining.values) value.clearSecrets();
        } else {
            QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
            auto remaining = f.vault.pendingRequests(f.vault.lease(), address); QCOMPARE(remaining.values.size(), 2);
            for (auto &value : remaining.values) { QVERIFY(value.messageId.isEmpty()); value.clearSecrets(); }
            QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        }
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void loadedCurrentRequestCannotContradictItsBoundPendingPair_data() {
        QTest::addColumn<QString>("field");
        QTest::newRow("device") << QString("device_id"); QTest::newRow("public-key") << QString("public_key");
        QTest::newRow("generation") << QString("key_uuid"); QTest::newRow("author") << QString("author_uuid");
    }
    void loadedCurrentRequestCannotContradictItsBoundPendingPair() {
        QFETCH(QString, field);
        Fixture f; QVERIFY(f.start());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted"); const auto grant = f.responseDto(source);
        auto changed = own; auto payload = changed.value("payload").toObject();
        if (field == "author_uuid") changed["author_uuid"] = Issuer;
        else payload[field] = field == "device_id" ? QString("different-device") : field == "key_uuid"
            ? OtherGeneration : QString::fromLatin1(QByteArray(32, 'q').toBase64());
        changed["payload"] = payload;
        const auto normal = f.http.handler;
        f.http.handler = [&, changed, grant, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{QJsonArray{changed, f.cipher, grant}};
            return normal(request);
        };
        f.controller.selectStream(streamId); QTRY_COMPARE(f.controller.messages().size(), 3);
        QTRY_COMPARE(f.bindingGets, 1); QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QCoreApplication::processEvents();
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 0);
    }

    void requestDeletionWhileAuthorityIsDelayedBlocksTheQueuedGrant() {
        Fixture f; QVERIFY(f.start());
        f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.streamDelay = 250;
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QTRY_COMPARE(f.streamGets, 1);
        auto removal = cryptoEvent(QJsonObject{{"uuid", own.value("uuid")}, {"project_id", projectId},
            {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}});
        auto payload = removal.value("payload").toObject(); payload["kind"] = "message.deleted"; removal["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(removal));
        QTRY_COMPARE(f.bindingGets, 1); QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        QCoreApplication::processEvents();
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
        QVERIFY(!f.vault.pendingRequests(f.vault.lease(), address).values.isEmpty());
        QCOMPARE(f.posts, 0);
    }

    void currentRequestUpdateReplacesTheObservationWithoutRefetchingAuthority() {
        Fixture f; QVERIFY(f.start()); f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.streamDelay = 150;
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        auto changed = own; changed["author_uuid"] = Issuer;
        auto event = cryptoEvent(changed); auto payload = event.value("payload").toObject();
        payload["kind"] = "message.updated"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event));
        const auto grant = f.responseDto(source); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant)));
        QTRY_COMPARE(f.bindingGets, 1); QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
        // A new verified current observation replaces the contradiction, not the
        // pending pair or UUID. The retained matching response can now complete.
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
        QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1); QCOMPARE(f.posts, 0);
    }

    void deletedRequestCannotBeResurrectedByALateCurrentPayloadReplay() {
        Fixture f; QVERIFY(f.start()); f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        auto event = cryptoEvent(QJsonObject{{"uuid", own.value("uuid")}, {"project_id", projectId},
            {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}});
        auto payload = event.value("payload").toObject(); payload["kind"] = "message.deleted"; event["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(event));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(own)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QTRY_COMPARE(f.bindingGets, 1); QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
        QCOMPARE(f.posts, 0);
    }

    void observedRequestCapacityCannotEvictANegativeFactAndAcceptAnOldGrant() {
        Fixture f; QVERIFY(f.start()); f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        NativeChatKeyVault::PendingRequest pending; QJsonObject body, own;
        QVERIFY(f.seedRequest(returnedId, "public", false, pending, body, own));
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        auto contradiction = own; contradiction["author_uuid"] = Issuer;
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(contradiction)));
        for (int index = 0; index < 500; ++index) {
            auto other = own; other["uuid"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(other)));
        }
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QTRY_COMPARE(f.bindingGets, 1); QTRY_COMPARE(f.api.findChildren<QNetworkReply *>().size(), 0);
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1);
    }

    void thirtyThreeLoadedResponsesFinishAfterSharedAuthorityCapacityFrees() {
        Fixture f; QVERIFY(f.start());
        f.controller.selectStream(streamId); QTRY_VERIFY(!f.controller.busy());
        QJsonArray grants; QStringList keys, locals;
        for (int index = 0; index < 33; ++index) {
            const auto key = QUuid::createUuid().toString(QUuid::WithoutBraces);
            auto pair = NativeChatCrypto::createRequestKeyPair(); QVERIFY(pair);
            const auto pairCleanup = qScopeGuard([&] { pair.privateKey.fill(0); pair.privateKey.clear(); });
            NativeChatKeyVault::PendingRequest pending{QUuid::createUuid().toString(QUuid::WithoutBraces), {},
                f.vault.device().name, pair.publicKey, pair.privateKey, index + 1, 0, {}, secondTopicId};
            const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
            pending.messageId = QUuid::createUuidV5(QUuid(secondTopicId), pending.localId.toUtf8()).toString(QUuid::WithoutBraces);
            QCOMPARE(f.vault.putPendingRequest(f.vault.lease(), {streamId, key}, pending), NativeChatKeyVault::Failure::None);
            const QJsonObject payload{{"kind", "key_request"}, {"key_uuid", key}, {"device_id", f.vault.device().id},
                {"device_name", pending.deviceName}, {"public_key", QString::fromLatin1(pending.publicKey.toBase64())}};
            auto own = message({}, pending.messageId, secondTopicId); own["payload"] = payload;
            const QJsonObject body{{"uuid", pending.localId}, {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}, {"payload", payload}};
            const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
            grants.append(f.responseDto(source)); keys.append(key); locals.append(pending.localId);
        }
        f.streamDelay = 350;
        QSignalSpy material(&f.vault, &NativeChatKeyVault::keyMaterialChanged);
        for (const auto &grant : grants) QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant.toObject())));
        QTRY_COMPARE(f.controller.messages().size(), 34);
        QTRY_COMPARE(material.size(), 33);
        for (int index = 0; index < keys.size(); ++index) {
            const NativeChatKeyVault::Address address{streamId, keys[index]};
            QVERIFY(f.vault.requestCompletion(f.vault.lease(), address, locals[index]).value.has_value());
            QVERIFY(f.vault.pendingRequests(f.vault.lease(), address).values.isEmpty());
        }
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void returnedRequestCannotChangeItsSavedImmutableDeviceLabel() {
        Fixture f;
        const auto normal = f.http.handler;
        f.http.handler = [normal](const Request &request) {
            auto response = normal(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/messages/") {
                auto dto = response.data.toObject(); auto payload = dto.value("payload").toObject();
                payload["device_name"] = "Different unverified label"; dto["payload"] = payload; response.data = dto;
            }
            return response;
        };
        QVERIFY(f.start()); f.controller.requestChatKey(returnedId);
        QTRY_COMPARE(f.posts, 1); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        auto pending = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        QVERIFY(pending.messageId.isEmpty());
        QCOMPARE(pending.deviceName, f.vault.device().name);
        QCOMPARE(f.controller.encryption().value("state").toString(), QString("unresolved"));
    }

    void queuedGrantCompletesAfterGetOnlyCheckBindsTheRequest() {
        Fixture f;
        const auto normal = f.http.handler;
        f.http.handler = [&](const Request &request) {
            auto response = normal(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                response.status = 503;
            return response;
        };
        QVERIFY(f.start()); f.controller.requestChatKey(returnedId);
        QTRY_COMPARE(f.posts, 1); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.controller.encryption().value("state").toString(), QString("unresolved"));
        const NativeChatKeyVault::Address address{streamId, returnedId};
        auto pending = f.vault.pendingRequests(f.vault.lease(), address).values.first();
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        QVERIFY(pending.messageId.isEmpty());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted");
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value);
        f.controller.checkChatKeyRequest(returnedId);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
        QVERIFY(f.vault.pendingRequests(f.vault.lease(), address).values.isEmpty());
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void loadedLegacyOwnRequestBindsAndCompletesActualSourceGrant_data() {
        QTest::addColumn<QString>("binding");
        for (const auto *value : {"unbound", "public", "legacy-local"}) QTest::newRow(value) << QString::fromLatin1(value);
    }
    void loadedLegacyOwnRequestBindsAndCompletesActualSourceGrant() {
        QFETCH(QString, binding);
        Fixture f; QVERIFY(f.start());
        auto pair = NativeChatCrypto::createRequestKeyPair(); QVERIFY(pair);
        const auto cleanupPair = qScopeGuard([&] { pair.privateKey.fill(0); pair.privateKey.clear(); });
        NativeChatKeyVault::PendingRequest pending{QUuid::createUuid().toString(QUuid::WithoutBraces), {},
            f.vault.device().name, pair.publicKey, pair.privateKey, 1234, 0, {}, {}};
        const auto cleanup = qScopeGuard([&] { pending.clearSecrets(); });
        const auto publicId = QUuid::createUuidV5(QUuid(secondTopicId), pending.localId.toUtf8()).toString(QUuid::WithoutBraces);
        if (binding == "public") pending.messageId = publicId;
        else if (binding == "legacy-local") pending.messageId = pending.localId;
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QCOMPARE(f.vault.putPendingRequest(f.vault.lease(), address, pending), NativeChatKeyVault::Failure::None);
        const QJsonObject payload{{"kind", "key_request"}, {"key_uuid", returnedId}, {"device_id", f.vault.device().id},
            {"device_name", pending.deviceName}, {"public_key", QString::fromLatin1(pending.publicKey.toBase64())}};
        auto own = message({}, publicId, secondTopicId); own["payload"] = payload;
        const QJsonObject body{{"uuid", pending.localId}, {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}, {"payload", payload}};
        const auto source = f.sourceResponseFor(body, own); QVERIFY(source.value("ok").toBool());
        f.cipher["payload"] = source.value("encrypted");
        const auto grant = f.responseDto(source);
        const auto normal = f.http.handler;
        f.http.handler = [&, own, grant, normal](const Request &request) {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return Response{QJsonArray{grant, f.cipher, own}};
            return normal(request);
        };
        f.controller.selectStream(streamId);
        QTRY_COMPARE(f.controller.messages().size(), 3);
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
        QVERIFY(f.vault.pendingRequests(f.vault.lease(), address).values.isEmpty());
        QTRY_COMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Synthetic source-granted plaintext"));
        QVERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 0); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void validImmutablePendingMetadataSurvivesOversizedCurrentDeviceLabel() {
        Fixture f; f.deviceName = QString(256, 'x'); QVERIFY(f.start());
        const auto pair = NativeChatCrypto::createRequestKeyPair(); QVERIFY(pair);
        NativeChatKeyVault::PendingRequest pending{QUuid::createUuid().toString(QUuid::WithoutBraces), {},
            "Previously saved immutable label", pair.publicKey, pair.privateKey, 1234, 0, {}, secondTopicId};
        pending.messageId = QUuid::createUuidV5(QUuid(secondTopicId), pending.localId.toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(f.vault.putPendingRequest(f.vault.lease(), {streamId, returnedId}, pending), NativeChatKeyVault::Failure::None);
        auto saved = message({}, pending.messageId, secondTopicId);
        saved["payload"] = QJsonObject{{"kind", "key_request"}, {"key_uuid", returnedId}, {"device_id", f.vault.device().id},
            {"device_name", pending.deviceName}, {"public_key", QString::fromLatin1(pending.publicKey.toBase64())}};
        f.savedRequests.insert(pending.messageId, saved);
        f.controller.checkChatKeyRequest(returnedId); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.controller.encryption().value("state").toString(), QString("requested")); QCOMPARE(f.posts, 0);
        const auto durable = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        QCOMPARE(durable.localId, pending.localId); QCOMPARE(durable.messageId, pending.messageId);
        QCOMPARE(durable.deviceName, pending.deviceName); QVERIFY(durable.privateKey == pair.privateKey);
    }

    void legitimateMemberRoleChangeDuringPreparationDoesNotStrandTheWindow() {
        Fixture f; QVERIFY(f.start()); QVERIFY(f.prepareA());
        for (int i = 1; i < 500; ++i) {
            auto row = f.cipher; row["uuid"] = QString("dddddddd-dddd-4ddd-8ddd-%1").arg(i, 12, 16, QLatin1Char('0'));
            QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(row)));
        }
        const auto count = [&] { int ready = 0; for (const auto &row : f.controller.messages()) ready += row.toMap().value("decrypted").toBool(); return ready; };
        bool changed = false;
        connect(&f.controller, &NativeController::messagesChanged, this, [&] {
            if (!changed && count() >= 8) {
                changed = true; auto event = selfBindingEvent("stream_binding.updated");
                auto payload = event.value("payload").toObject(); payload["role"] = "moderator"; event["payload"] = payload;
                QVERIFY(f.controller.applyRealtimeEvent(event));
            }
        });
        QTRY_VERIFY(changed); QTRY_COMPARE_WITH_TIMEOUT(count(), 500, 2000);
        QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1); QCOMPARE(f.posts, 1);
    }

    void latePostFromDeletedOperationCannotReplaceNewBusyOperation() {
        Fixture f; f.postDelay = 200; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QPointer<QNetworkReply> old;
        for (auto *reply : f.api.findChildren<QNetworkReply *>()) if (reply->url().path().endsWith("/messages/")) old = reply;
        QVERIFY(old); bool finished = false; connect(old, &QNetworkReply::finished, this, [&] { finished = true; });
        QCOMPARE(f.vault.remove(f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
        f.postDelay = 600; f.controller.requestChatKey(OtherGeneration); QTRY_COMPARE(f.posts, 2);
        QTRY_VERIFY(finished);
        QCOMPARE(f.controller.encryption().value("keyUuid").toString(), OtherGeneration);
        QVERIFY(f.controller.encryption().value("busy").toBool()); QVERIFY(f.controller.encryption().value("error").toString().isEmpty());
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool()); QCOMPARE(f.posts, 2);
    }

    void lateStatusGetCannotPublishOverNewOperation() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        f.checkDelay = 200; f.controller.checkChatKeyRequest(returnedId);
        QPointer<QNetworkReply> old;
        for (auto *reply : f.api.findChildren<QNetworkReply *>()) if (reply->url().path().endsWith(f.savedRequest.value("uuid").toString())) old = reply;
        QVERIFY(old); bool finished = false; connect(old, &QNetworkReply::finished, this, [&] { finished = true; });
        QCOMPARE(f.vault.remove(f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
        f.postDelay = 600; f.controller.requestChatKey(OtherGeneration); QTRY_COMPARE(f.posts, 2);
        QTRY_VERIFY(finished);
        QCOMPARE(f.controller.encryption().value("keyUuid").toString(), OtherGeneration);
        QVERIFY(f.controller.encryption().value("busy").toBool()); QVERIFY(f.controller.encryption().value("error").toString().isEmpty());
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool()); QCOMPARE(f.posts, 2);
    }

    void vaultPublicationSubscriberCanAdmitBWithoutOldAOverwrite() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool()); bool began = false;
        connect(&f.vault, &NativeChatKeyVault::keyMaterialChanged, this, [&](const QString &, const QString &key) {
            if (key == returnedId && !began) { began = true; f.postDelay = 600; f.controller.requestChatKey(OtherGeneration); }
        });
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source)))); QVERIFY(began);
        QCOMPARE(f.controller.encryption().value("keyUuid").toString(), OtherGeneration);
        QVERIFY(f.controller.encryption().value("busy").toBool());
        QTRY_COMPARE(f.posts, 2); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
    }

    void valid255CodepointLabelUses256Utf16UnitsWithoutRewrite() {
        Fixture f; f.deviceName = QString(254, 'x') + QString::fromUtf8("\xf0\x9f\x98\x80");
        QCOMPARE(f.deviceName.size(), 256); QCOMPARE(f.deviceName.toUcs4().size(), 255);
        QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.requestBody.value("payload").toObject().value("device_name").toString(), f.deviceName);
        const auto saved = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        f.controller.checkChatKeyRequest(returnedId); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().deviceName, saved.deviceName);
        QCOMPARE(f.posts, 1);
    }

    void revocationSurvivesRouteAndStaleBindingUntilFreshCatalog() {
        Fixture f; QVERIFY(f.start()); QVERIFY(f.prepareA());
        QVERIFY(f.controller.applyRealtimeEvent(selfBindingEvent("stream_binding.deleted")));
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        f.controller.selectStream({}); f.controller.selectStream(streamId); QTRY_COMPARE(f.controller.topics().size(), 2);
        f.controller.selectTopic(topicId); QTRY_VERIFY(!f.controller.busy());
        QVERIFY(f.controller.applyRealtimeEvent(selfBindingEvent("stream_binding.updated")));
        auto counter = cryptoEvent(f.stream); auto payload = counter.value("payload").toObject(); payload["kind"] = "stream.read"; counter["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(counter));
        bool drained = false; QTimer::singleShot(0, this, [&] { drained = true; }); QTRY_VERIFY(drained);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        f.controller.reloadSnapshot(); QTRY_VERIFY(!f.controller.busy());
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool()); QCOMPARE(f.posts, 1);
    }

    void inFlightCatalogFromBeforeSecondRevocationCannotRestoreAccess() {
        Fixture f; QVERIFY(f.start()); QVERIFY(f.prepareA());
        QVERIFY(f.controller.applyRealtimeEvent(selfBindingEvent("stream_binding.deleted")));
        f.catalogDelay = 200; f.controller.reloadSnapshot(); QTRY_COMPARE(f.catalogGets, 2);
        QVERIFY(f.controller.applyRealtimeEvent(selfBindingEvent("stream_binding.updated")));
        QVERIFY(f.controller.applyRealtimeEvent(selfBindingEvent("stream_binding.deleted")));
        QTRY_VERIFY(!f.controller.busy());
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        f.catalogDelay = 0; f.controller.reloadSnapshot(); QTRY_VERIFY(!f.controller.busy());
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool()); QCOMPARE(f.posts, 1);
    }

    void authority503PreservesLocalReadBut403RevokesIt() {
        Fixture f; QVERIFY(f.start()); QVERIFY(f.prepareA());
        f.controller.useChatKeys(&f.vault); QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        const auto reads = f.streamGets; f.streamStatus = 503;
        f.controller.requestChatKey(OtherGeneration); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.streamGets, reads + 1); QVERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        f.streamStatus = 403; f.controller.requestChatKey(OtherGeneration); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool()); QCOMPARE(f.posts, 1);
    }

    void lateValidGenerationACompletesWithoutClobberingActiveB() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto pendingA = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        const auto sourceA = f.sourceResponse(); QVERIFY(sourceA.value("ok").toBool());
        f.cipher["payload"] = sourceA.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        f.postDelay = 600; f.controller.requestChatKey(OtherGeneration); QTRY_COMPARE(f.posts, 2);
        const auto before = f.controller.encryption(); QVERIFY(before.value("busy").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(sourceA))));
        QVERIFY(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pendingA.localId).value.has_value());
        QCOMPARE(f.controller.encryption().value("keyUuid"), before.value("keyUuid"));
        QCOMPARE(f.controller.encryption().value("state"), before.value("state"));
        QCOMPARE(f.controller.encryption().value("busy"), before.value("busy"));
        QCOMPARE(f.controller.encryption().value("error"), before.value("error"));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QTRY_VERIFY(!f.controller.encryption().value("busy").toBool()); QCOMPARE(f.posts, 2);
    }

    void admissionSubscriberReplacementOnlyDispatchesNewOperation() {
        Fixture f; QVERIFY(f.start()); bool replaced = false;
        connect(&f.controller, &NativeController::stateChanged, this, [&] {
            const auto state = f.controller.encryption();
            if (!replaced && state.value("keyUuid").toString() == returnedId && state.value("state").toString() == "verifying") {
                replaced = true;
                QCOMPARE(f.vault.remove(f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
                f.controller.requestChatKey(OtherGeneration);
            }
        });
        f.controller.requestChatKey(returnedId); QTRY_VERIFY(replaced); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.posts, 1); QCOMPARE(f.requestBodies.first().value("payload").toObject().value("key_uuid").toString(), OtherGeneration);
        QVERIFY(f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.isEmpty());
    }

    void selfBindingDeletionClearsPreparedBeforeNotificationsAndBlocksQueuedWork() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        f.cipher["payload"] = source.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        bool notified = false;
        connect(&f.controller, &NativeController::stateChanged, this, [&] { notified = true; QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool()); });
        auto revoked = cryptoEvent(QJsonObject{{"uuid", messageId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"user_uuid", userId}});
        auto payload = revoked.value("payload").toObject(); payload["kind"] = "stream_binding.deleted"; revoked["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(revoked)); QVERIFY(notified);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        bool drained = false; QTimer::singleShot(0, this, [&] { drained = true; }); QTRY_VERIFY(drained);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
    }

    void explicitHistoricalGenerationWorksWhenCurrentIsNull() {
        Fixture f; f.stream["current_encryption_key_uuid"] = QJsonValue::Null; QVERIFY(f.start());
        f.controller.requestChatKey(); QCOMPARE(f.posts, 0);
        f.controller.requestChatKey(returnedId); QTRY_COMPARE_WITH_TIMEOUT(f.posts, 1, 2000);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        QCOMPARE(f.requestBody.value("payload").toObject().value("key_uuid").toString(), returnedId);
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        f.cipher["payload"] = source.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void oversizedDeviceLabelIsRejectedBeforeRequestPersistence() {
        Fixture f; f.deviceName = QString(256, 'x'); QVERIFY(f.start());
        f.controller.requestChatKey(returnedId); QTRY_VERIFY(!f.controller.encryption().value("busy").toBool());
        QCOMPARE(f.posts, 0); QVERIFY(f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.isEmpty());
        QVERIFY(!f.controller.encryption().value("error").toString().isEmpty());
    }

    void freshOwnedCatalogRemovalCannotBeUndoneByStaleStreamAdd() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        f.cipher["payload"] = source.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        f.catalogOwnsStream = false; f.controller.reloadSnapshot(); QTRY_VERIFY(!f.controller.busy());
        QVERIFY(f.controller.selectedStream().isEmpty());
        auto stale = cryptoEvent(f.stream); auto payload = stale.value("payload").toObject(); payload["kind"] = "stream.updated"; stale["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(stale)); f.controller.selectStream(streamId);
        QTRY_COMPARE(f.controller.topics().size(), 2); f.controller.selectTopic(topicId); QTRY_VERIFY(!f.controller.busy());
        bool drained = false; QTimer::singleShot(0, this, [&] { drained = true; }); QTRY_VERIFY(drained);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
    }

    void reentrantLocalDeletionCannotPublishGrantAsReady() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        bool removed = false;
        connect(&f.vault, &NativeChatKeyVault::keyMaterialChanged, this, [&](const QString &, const QString &) {
            if (!removed) { removed = true; QCOMPARE(f.vault.remove(f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None); }
        });
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source)))); QVERIFY(removed);
        QVERIFY(f.controller.encryption().value("state").toString() != "ready");
        QVERIFY(f.vault.read(f.vault.lease(), {streamId, returnedId}).value.bytes.isEmpty());
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
    }

    void historicalLocalKeyPreparesWithoutNewAuthorityOrNetwork() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto output = f.sourceResponse(); QVERIFY(output.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(output))));
        f.cipher["payload"] = output.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        const auto requests = f.http.requests.size();
        auto stream = f.stream; stream["current_encryption_key_uuid"] = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        auto changed = cryptoEvent(stream); auto payload = changed.value("payload").toObject(); payload["kind"] = "stream.updated"; changed["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(changed)); f.controller.useChatKeys(&f.vault);
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(f.http.requests.size(), requests); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1); QCOMPARE(f.posts, 1);
    }

    void grantPublicationSubscriberCanDestroyBothControllerAndVault() {
        auto f = std::make_unique<Fixture>(); QVERIFY(f->start()); f->controller.requestChatKey(returnedId); QTRY_COMPARE(f->posts, 1);
        QTRY_VERIFY(!f->vault.pendingRequests(f->vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto source = f->sourceResponse(); QVERIFY(source.value("ok").toBool());
        const auto grant = f->responseDto(source);
        QPointer<NativeController> controller(&f->controller); QPointer<NativeChatKeyVault> vault(&f->vault);
        connect(controller, &NativeController::messagesChanged, this, [&] {
            if (f && f->vault.read(f->vault.lease(), {streamId, returnedId}).value.bytes.size() == 32) f.reset();
        });
        QVERIFY(controller->applyRealtimeEvent(cryptoEvent(grant)));
        QVERIFY(!controller); QVERIFY(!vault); QVERIFY(!f);
    }

    void oversizedUtf16ReplacementDoesNotRetainPartialPlaintext() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto output = f.sourceResponse(); QVERIFY(output.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(output))));
        f.cipher["payload"] = output.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        auto key = f.vault.read(f.vault.lease(), {streamId, returnedId});
        const auto text = QString::fromUtf8("\xf0\x9f\x98\x80").repeated(2000); QCOMPARE(text.size(), 4000);
        const auto ciphertext = NativeChatCrypto::encryptMessage(key.value.bytes, text.toUtf8(), {streamId, returnedId});
        key.value.bytes.fill(0); QVERIFY(ciphertext);
        auto payload = f.cipher.value("payload").toObject(); payload["content"] = QString::fromLatin1(ciphertext.bytes.toBase64());
        for (int i = 0; i < 500; ++i) {
            auto row = f.cipher; row["payload"] = payload;
            if (i) row["uuid"] = QString("dddddddd-dddd-4ddd-8ddd-%1").arg(i, 12, 16, QLatin1Char('0'));
            QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(row)));
        }
        QSignalSpy changed(&f.controller, &NativeController::messagesChanged);
        QTRY_VERIFY(!changed.isEmpty());
        const auto count = [&] { int ready = 0; for (const auto &row : f.controller.messages()) ready += row.toMap().value("decrypted").toBool(); return ready; };
        QTRY_COMPARE(count(), 0);
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void actualSourceRejectCommitsReceiptWithoutMaterialNotification() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto pending = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        QSignalSpy changes(&f.vault, &NativeChatKeyVault::keyMaterialChanged);
        const auto output = f.sourceResponse(true); QVERIFY(output.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(output))));
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value.has_value());
        QCOMPARE(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value->outcome,
            NativeChatKeyVault::Outcome::Rejected);
        QVERIFY(changes.isEmpty()); QVERIFY(f.vault.read(f.vault.lease(), {streamId, returnedId}).value.bytes.isEmpty());
        QVERIFY(f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.isEmpty());
        QCOMPARE(f.controller.encryption().value("state").toString(), QString("rejected")); QCOMPARE(f.posts, 1);
    }

    void malformedBase64AndWrongRecipientCannotCommitTheGrant() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto pending = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        const auto output = f.sourceResponse(); QVERIFY(output.value("ok").toBool());
        auto grant = f.responseDto(output); auto payload = grant.value("payload").toObject();
        payload["content"] = payload.value("content").toString() + '\n'; grant["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant)));
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        grant = f.responseDto(output); payload = grant.value("payload").toObject(); payload["recipient_device_id"] = IssuerDevice;
        grant["payload"] = payload; QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant)));
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(output))));
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value.has_value());
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void legacyCatalogNeedsFreshAuthorityAndConcurrentResponsesShareIt() {
        Fixture f; f.legacyCatalog = true; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto pending = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        const auto output = f.sourceResponse(); QVERIFY(output.value("ok").toBool());
        f.controller.useChatKeys(&f.vault); f.streamDelay = 120;
        QSignalSpy changes(&f.vault, &NativeChatKeyVault::keyMaterialChanged);
        auto grant = f.responseDto(output);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant)));
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value.has_value());
        QCOMPARE(f.streamGets, 2); QCOMPARE(f.bindingGets, 2); QCOMPARE(changes.size(), 1); QCOMPARE(f.posts, 1);
    }

    void metadataOnlyUpdatesKeepPreparedBodyButCiphertextUpdateCannotShowStalePlaintext() {
        Fixture f; QVERIFY(f.start()); QVERIFY(f.prepareA());
        const auto requests = f.http.requests.size();
        auto metadata = f.cipher; metadata["starred"] = true;
        auto event = cryptoEvent(metadata); auto envelope = event.value("payload").toObject();
        envelope["kind"] = "message.updated"; event["payload"] = envelope;
        QVERIFY(f.controller.applyRealtimeEvent(event));
        QCOMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Synthetic source-granted plaintext"));
        QVERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        auto key = f.vault.read(f.vault.lease(), {streamId, returnedId});
        const auto cleanup = qScopeGuard([&] { key.value.bytes.fill(0); key.value.bytes.clear(); });
        auto encoded = NativeChatCrypto::encryptMessage(key.value.bytes, QByteArray("Changed actual ciphertext body"), {streamId, returnedId});
        QVERIFY(encoded); auto payload = metadata.value("payload").toObject();
        payload["content"] = QString::fromLatin1(encoded.bytes.toBase64()); metadata["payload"] = payload;
        event = cryptoEvent(metadata); envelope = event.value("payload").toObject(); envelope["kind"] = "message.updated";
        event["payload"] = envelope; QVERIFY(f.controller.applyRealtimeEvent(event));
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        QVERIFY(!displayed(f.controller, messageId).value("content").toString().contains("Synthetic source-granted plaintext"));
        QTRY_COMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Changed actual ciphertext body"));
        QCOMPARE(f.http.requests.size(), requests);
    }

    void equalCiphertextRepreparesAfterCommittedMaterialRemovalAndReimport() {
        Fixture f; QVERIFY(f.start()); QVERIFY(f.prepareA());
        const auto requests = f.http.requests.size(); const NativeChatKeyVault::Address address{streamId, returnedId};
        auto material = f.vault.read(f.vault.lease(), address);
        const auto cleanup = qScopeGuard([&] { material.value.bytes.fill(0); material.value.bytes.clear(); });
        QCOMPARE(material.value.bytes.size(), 32);
        QCOMPARE(f.vault.remove(f.vault.lease(), address), NativeChatKeyVault::Failure::None);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        const auto empty = f.vault.read(f.vault.lease(), address); QVERIFY(empty.value.bytes.isEmpty());
        QCOMPARE(f.vault.put(f.vault.lease(), address, material.value.bytes, empty.value.deletionRevision), NativeChatKeyVault::Failure::None);
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QCOMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Synthetic source-granted plaintext"));
        QCOMPARE(f.http.requests.size(), requests);
    }

    void preparedFiveHundredRowsYieldAndOversizedWindowDropsAll() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto output = f.sourceResponse(); QVERIFY(output.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(output))));
        f.cipher["payload"] = output.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        const auto count = [&] { int ready = 0; for (const auto &row : f.controller.messages()) ready += row.toMap().value("decrypted").toBool(); return ready; };
        const auto row = [&](int number) {
            auto value = f.cipher;
            value["uuid"] = QString("dddddddd-dddd-4ddd-8ddd-%1").arg(number, 12, 16, QLatin1Char('0'));
            return value;
        };
        for (int i = 1; i < 500; ++i) QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(row(i))));
        QElapsedTimer timer; timer.start(); qint64 previous = 0, maximum = 0; int pulses = 0;
        QTimer pulse; pulse.setInterval(2);
        connect(&pulse, &QTimer::timeout, this, [&] { const auto now = timer.nsecsElapsed(); maximum = std::max(maximum, now - previous); previous = now; ++pulses; });
        pulse.start(); QTRY_COMPARE(count(), 500); pulse.stop();
        qInfo("500-row preparation: %lld ms total, max pulse %.3f ms, %d pulses", timer.elapsed(), maximum / 1000000.0, pulses);
        QVERIFY(pulses > 2); QVERIFY(maximum < 100000000); // meaningful GUI responsiveness, not a crypto timing contract
        const auto network = f.http.requests.size();
        for (int i = 0; i < 10; ++i) { f.controller.encryption(); f.controller.messages(); }
        QCOMPARE(f.http.requests.size(), network);
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(row(500)))); QTRY_COMPARE(count(), 0);
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void administratorRoleLossInvalidatesAuthorityBeforeGrant() {
        Fixture f; f.stream["owner"] = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"; f.issuerRole = "administrator";
        QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto pending = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first();
        const auto source = f.sourceResponse(); QVERIFY(source.value("ok").toBool());
        f.issuerRole = "member";
        auto changed = cryptoEvent(QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"user_uuid", Issuer}, {"who_uuid", userId}, {"role", "member"}, {"notification_mode", "all_messages"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:01:00Z"}});
        auto payload = changed.value("payload").toObject(); payload["kind"] = "stream_binding.updated"; changed["payload"] = payload;
        QVERIFY(f.controller.applyRealtimeEvent(changed));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(source))));
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        QTRY_COMPARE(f.streamGets, 2);
        QVERIFY(f.vault.read(f.vault.lease(), {streamId, returnedId}).value.bytes.isEmpty());
        QCOMPARE(f.posts, 1);
    }

    void localKeyRemovalImmediatelyPublishesLockedModel() {
        Fixture f; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first().messageId.isEmpty());
        const auto output = f.sourceResponse(); QVERIFY(output.value("ok").toBool());
        f.cipher["payload"] = output.value("encrypted"); QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(output))));
        QTRY_VERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        QSignalSpy changed(&f.controller, &NativeController::messagesChanged);
        QCOMPARE(f.vault.remove(f.vault.lease(), {streamId, returnedId}), NativeChatKeyVault::Failure::None);
        QVERIFY(!displayed(f.controller, messageId).value("decrypted").toBool());
        QTRY_VERIFY(!changed.isEmpty());
        QVERIFY(!displayed(f.controller, messageId).value("content").toString().contains("Synthetic source-granted plaintext"));
    }

    void responseBeforePostConfirmationWaitsForExactBinding() {
        Fixture f; f.postDelay = 600; QVERIFY(f.start()); f.controller.requestChatKey(returnedId); QTRY_COMPARE(f.posts, 1);
        const auto pending = f.vault.pendingRequests(f.vault.lease(), {streamId, returnedId}).values.first(); QVERIFY(pending.messageId.isEmpty());
        const auto output = f.sourceResponse(); QVERIFY(output.value("ok").toBool());
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.responseDto(output))));
        QVERIFY(!f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value);
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), {streamId, returnedId}, pending.localId).value.has_value());
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
    }

    void realTlsRequestActualSourceGrantAndScopedDecrypt() {
        Fixture f; QVERIFY(f.start()); QVERIFY(f.api.server().scheme() == "https");
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
        f.controller.requestChatKey(returnedId);
        QTRY_COMPARE(f.posts, 1);
        QCOMPARE(f.requestBody.value("stream_uuid").toString(), streamId);
        QCOMPARE(f.requestBody.value("topic_uuid").toString(), secondTopicId);
        const auto payload = f.requestBody.value("payload").toObject();
        QCOMPARE(payload.value("kind").toString(), QString("key_request"));
        const NativeChatKeyVault::Address address{streamId, returnedId};
        QTRY_COMPARE(f.vault.pendingRequests(f.vault.lease(), address).values.size(), 1);
        QTRY_VERIFY(!f.vault.pendingRequests(f.vault.lease(), address).values.first().messageId.isEmpty());
        const auto pending = f.vault.pendingRequests(f.vault.lease(), address).values.first();
        QCOMPARE(pending.targetTopic, secondTopicId);
        QCOMPARE(pending.messageId, f.savedRequest.value("uuid").toString());
        QProcess peer;
        peer.start(QStringLiteral(WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE), {QStringLiteral(WORKSPACE_NATIVE_E2EE_PEER_SCRIPT)}); QVERIFY(peer.waitForStarted());
        const QJsonObject input{{"project", projectId}, {"stream", streamId}, {"key", returnedId},
            {"request", pending.messageId}, {"requester", userId}, {"author", Issuer}, {"senderDevice", IssuerDevice},
            {"recipientDevice", payload.value("device_id")}, {"publicKey", payload.value("public_key")},
            {"role", "owner"}, {"plaintext", "Synthetic source-granted plaintext"}};
        peer.write(QJsonDocument(input).toJson(QJsonDocument::Compact)); peer.closeWriteChannel();
        QVERIFY(peer.waitForFinished(10000)); QCOMPARE(peer.exitCode(), 0);
        const auto output = QJsonDocument::fromJson(peer.readAllStandardOutput()).object(); QVERIFY(output.value("ok").toBool());
        f.cipher["payload"] = output.value("encrypted");
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(f.cipher)));
        const auto response = output.value("message").toObject();
        auto grant = message({}, response.value("uuid").toString(), secondTopicId);
        grant["author_uuid"] = Issuer; grant["is_own"] = false; grant["payload"] = response.value("payload");
        QVERIFY(f.controller.applyRealtimeEvent(cryptoEvent(grant)));
        QTRY_VERIFY(f.vault.requestCompletion(f.vault.lease(), address, pending.localId).value.has_value());
        QVERIFY(f.vault.pendingRequests(f.vault.lease(), address).values.isEmpty());
        QTRY_COMPARE(displayed(f.controller, messageId).value("content").toString(), QString("Synthetic source-granted plaintext"));
        QVERIFY(displayed(f.controller, messageId).value("decrypted").toBool());
        const auto raw = parseMessage(f.cipher, projectId); QVERIFY(raw); QVERIFY(raw->content.isEmpty());
        QDirIterator files(f.directory.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) { QFile file(files.next()); QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(!file.readAll().contains("Synthetic source-granted plaintext")); }
        QCOMPARE(f.posts, 1); QCOMPARE(f.streamGets, 1); QCOMPARE(f.bindingGets, 1);
        QVERIFY(!f.controller.canSend()); QVERIFY(!f.controller.canAttach());
    }
};
int main(int argc, char **argv)
{
#ifdef WORKSPACE_NATIVE_E2EE_UI
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
#else
    QCoreApplication app(argc, argv);
#endif
    if (app.arguments().size() == 2 && app.arguments().at(1) == "--process-worker") {
        qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &) {
            if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) ++ProcessWarnings;
        });
        QJsonObject status; const auto result = processWorker(status);
        const auto output = QJsonDocument(status).toJson(QJsonDocument::Compact);
        std::fwrite(output.constData(), 1, size_t(output.size()), stdout); std::fflush(stdout);
        return result;
    }
    NativeE2eeReadTest test; return QTest::qExec(&test, argc, argv);
}
#include "native-e2ee-read-test.moc"
