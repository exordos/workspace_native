#include "api-client.h"
#include "file-client.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

using namespace workspace;

namespace {
const QString FileUuid = "047dd8a7-b556-421a-851e-dbf22971834a";
const QString StreamUuid = "d3664145-b601-4245-a109-226c324e968f";
const QByteArray Bytes("binary\0payload\xff\n", 16);
QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
QByteArray metadata(const QByteArray &bytes = Bytes)
{
    return QJsonDocument(QJsonObject{{"uuid", FileUuid}, {"stream_uuid", StreamUuid},
        {"name", "attachment.bin"}, {"content_type", "application/octet-stream"},
        {"size_bytes", bytes.size()}, {"hash", QString::fromLatin1(digest(bytes))}}).toJson(QJsonDocument::Compact);
}
QByteArray header(const QByteArray &request, const QByteArray &key)
{
    for (const auto &line : request.left(request.indexOf("\r\n\r\n")).split('\n'))
        if (line.toLower().startsWith(key.toLower() + ':')) return line.mid(line.indexOf(':') + 1).trimmed();
    return {};
}
class Server : public QObject {
public:
    Server()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto &buffer = buffers[socket];
                    buffer += socket->readAll();
                    const auto end = buffer.indexOf("\r\n\r\n");
                    if (end < 0 || buffer.size() < end + 4 + header(buffer, "Content-Length").toLongLong()) return;
                    requests.append(buffer);
                    buffers.remove(socket);
                    if (handler) handler(socket, requests.last());
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] { buffers.remove(socket); socket->deleteLater(); });
            }
        });
    }
    ~Server() override { for (auto *socket : server.findChildren<QTcpSocket *>()) socket->abort(); }
    QUrl origin() const { return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())); }
    static void reply(QTcpSocket *socket, const QByteArray &body, int status = 200, const QByteArray &extra = {})
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
        socket->disconnectFromHost();
    }
    QTcpServer server;
    QHash<QTcpSocket *, QByteArray> buffers;
    QList<QByteArray> requests;
    std::function<void(QTcpSocket *, const QByteArray &)> handler;
};
void authorize(ApiClient &api, Server &server)
{
    api.setServer(server.origin());
    api.setAccessToken("file-test-token");
}
QByteArray read(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
void write(const QString &path, const QByteArray &bytes) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); }
}

class FileClientTest : public QObject {
    Q_OBJECT
private slots:
    void multipartUpload();
    void downloadAclAndAtomicCommit();
    void rejectedDownloads_data();
    void rejectedDownloads();
    void generationCancelsPartialDownload();
    void cancellationAndDestruction();
    void uploadBoundsAndFailures();
    void noRedirectedBearerOrCookies();
    void writeErrorsAndDeadline();
    void absentChecksumAndEmptyDownload();
    void typedUrns();
    void streamingAcrossMultipleChunks();
    void apiDestructionCancelsTransfers();
    void realWriteFailurePreservesDestination();
    void memoryDownloadReturnsBytes_data();
    void memoryDownloadReturnsBytes();
    void memoryDownloadRejectsUnsafeTransfers_data();
    void memoryDownloadRejectsUnsafeTransfers();
    void memoryDownloadLifetime_data();
    void memoryDownloadLifetime();
    void memoryCancellationDoesNotCancelAnotherOperation();
    void memoryCompletionMayDestroyItsClient();
};

void FileClientTest::multipartUpload()
{
    Server server;
    server.handler = [](QTcpSocket *socket, const QByteArray &) { Server::reply(socket, metadata(), 201); };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    const auto source = directory.filePath("private-local-name.bin"); write(source, Bytes);
    FileResult result; bool done = false;
    client.upload(source, StreamUuid, "attachment.bin", [&](const FileResult &r) { result = r; done = true; });
    QTRY_VERIFY(done);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.metadata.uuid, FileUuid);
    QVERIFY(result.checksumVerified);
    QCOMPARE(server.requests.size(), 1);
    const auto request = server.requests.first();
    QVERIFY(request.startsWith("POST /api/workspace/v1/messenger/files/ HTTP/1.1"));
    QCOMPARE(header(request, "Authorization"), "Bearer file-test-token");
    QVERIFY(header(request, "Content-Type").startsWith("multipart/form-data; boundary="));
    QVERIFY(request.contains("name=\"file\"; filename=\"attachment.bin\""));
    QVERIFY(request.contains("name=\"stream_uuid\"\r\n\r\n" + StreamUuid.toUtf8()));
    QVERIFY(request.contains("name=\"name\"\r\n\r\nattachment.bin"));
    QVERIFY(request.contains(Bytes));
    QVERIFY(!request.contains(source.toUtf8()));
}

void FileClientTest::downloadAclAndAtomicCommit()
{
    Server server;
    server.handler = [](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.contains("/actions/download ") ? Bytes : metadata());
    };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    const auto destination = directory.filePath("saved.bin"); write(destination, "old data");
    FileResult result; bool done = false;
    client.download(FileUuid, destination, [&](const FileResult &r) { result = r; done = true; });
    QTRY_VERIFY(done);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(read(destination), Bytes);
    QVERIFY(result.checksumVerified);
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(server.requests[0].startsWith("GET /api/workspace/v1/messenger/files/" + FileUuid.toUtf8() + " HTTP/1.1"));
    QVERIFY(server.requests[1].startsWith("GET /api/workspace/v1/messenger/files/" + FileUuid.toUtf8() + "/actions/download HTTP/1.1"));
    for (const auto &request : server.requests) QCOMPARE(header(request, "Authorization"), "Bearer file-test-token");
}

void FileClientTest::rejectedDownloads_data()
{
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario : {"acl403", "metadataRedirect", "downloadRedirect", "wrongUuid",
             "fractionalSize", "negativeSize", "unsafeSize", "invalidHash", "invalidJson",
             "metadataOverflow", "declaredOverflow", "streamOverflow", "truncated", "badHash"})
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
}

void FileClientTest::rejectedDownloads()
{
    QFETCH(QString, scenario);
    Server server;
    auto object = QJsonDocument::fromJson(metadata()).object();
    if (scenario == "wrongUuid") object["uuid"] = StreamUuid;
    if (scenario == "fractionalSize") object["size_bytes"] = 1.5;
    if (scenario == "negativeSize") object["size_bytes"] = -1;
    if (scenario == "unsafeSize") object["size_bytes"] = 9007199254740992.0;
    if (scenario == "invalidHash") object["hash"] = "unsupported-or-corrupt-hash";
    if (scenario == "metadataOverflow") object["description"] = QString(1024, 'x');
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!request.contains("/actions/download ")) {
            if (scenario == "acl403") { Server::reply(socket, "private details", 403); return; }
            if (scenario == "metadataRedirect") { Server::reply(socket, "", 302, "Location: /elsewhere\r\n"); return; }
            Server::reply(socket, scenario == "invalidJson" ? "not json" : QJsonDocument(object).toJson(QJsonDocument::Compact));
        } else {
            if (scenario == "downloadRedirect") { Server::reply(socket, "", 302, "Location: /elsewhere\r\n"); return; }
            if (scenario == "declaredOverflow") { Server::reply(socket, Bytes + 'x'); return; }
            if (scenario == "streamOverflow") {
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + Bytes + 'x'); socket->disconnectFromHost(); return;
            }
            if (scenario == "truncated") {
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Bytes.size()) + "\r\nConnection: close\r\n\r\n" + Bytes.left(3)); socket->disconnectFromHost(); return;
            }
            Server::reply(socket, scenario == "badHash" ? QByteArray(Bytes.size(), 'x') : Bytes);
        }
    };
    ApiClient api(nullptr, true); authorize(api, server);
    FileTransferLimits limits; limits.metadataBytes = 512;
    FileClient client(&api, nullptr, limits);
    QTemporaryDir directory;
    const auto destination = directory.filePath("saved.bin"); write(destination, "original");
    FileResult result; bool done = false;
    client.download(FileUuid, destination, [&](const FileResult &r) { result = r; done = true; });
    QTRY_VERIFY(done);
    QVERIFY(!result.ok()); QVERIFY(!result.error.isEmpty());
    QVERIFY(!result.error.contains("private details"));
    QCOMPARE(read(destination), "original");
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"saved.bin"});
    const auto expectBinary = scenario == "downloadRedirect" || scenario == "declaredOverflow"
        || scenario == "streamOverflow" || scenario == "truncated" || scenario == "badHash";
    QCOMPARE(server.requests.size(), expectBinary ? 2 : 1);
}

void FileClientTest::generationCancelsPartialDownload()
{
    Server server;
    QPointer<QTcpSocket> held;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!request.contains("/actions/download ")) Server::reply(socket, metadata());
        else {
            held = socket;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Bytes.size()) + "\r\n\r\n" + Bytes.left(3));
        }
    };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    const auto destination = directory.filePath("saved.bin"); write(destination, "original");
    FileResult result; int completions = 0;
    client.download(FileUuid, destination, [&](const FileResult &r) { result = r; ++completions; });
    QTRY_VERIFY(held);
    api.setAccessToken("another-account-token");
    QTRY_COMPARE(completions, 1);
    QVERIFY(!result.ok()); QCOMPARE(result.status, 0);
    if (held) { held->write(Bytes.mid(3)); held->disconnectFromHost(); }
    QCoreApplication::processEvents();
    QCOMPARE(completions, 1);
    QCOMPARE(read(destination), "original");
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"saved.bin"});
}

void FileClientTest::cancellationAndDestruction()
{
    Server server;
    QPointer<QTcpSocket> held;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!request.contains("/actions/download ")) Server::reply(socket, metadata());
        else held = socket;
    };
    ApiClient api(nullptr, true); authorize(api, server);
    QTemporaryDir directory;
    const auto destination = directory.filePath("saved.bin");
    int completions = 0;
    auto client = std::make_unique<FileClient>(&api);
    auto id = client->download(FileUuid, destination, [&](const FileResult &r) { QVERIFY(!r.ok()); ++completions; });
    QTRY_VERIFY(held);
    client->cancel(id);
    QCOMPARE(completions, 1);
    QVERIFY(!QFile::exists(destination));
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
    held.clear();
    client->download(FileUuid, destination, [&](const FileResult &) { ++completions; });
    QTRY_VERIFY(held);
    client.reset();
    QTRY_VERIFY(!held || held->state() == QAbstractSocket::UnconnectedState);
    QCOMPARE(completions, 1);
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
}

void FileClientTest::uploadBoundsAndFailures()
{
    Server server;
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    const auto source = directory.filePath("source.bin"); write(source, "");
    FileResult result; int completions = 0;
    const auto done = [&](const FileResult &r) { result = r; ++completions; };
    client.upload(source, StreamUuid, "attachment.bin", done);
    QTRY_COMPARE(completions, 1); QVERIFY(!result.ok());
    { QFile sparse(source); QVERIFY(sparse.open(QIODevice::WriteOnly)); QVERIFY(sparse.resize(25 * 1024 * 1024 + 1)); }
    client.upload(source, StreamUuid, "attachment.bin", done);
    QTRY_COMPARE(completions, 2); QVERIFY(!result.ok());
    QCOMPARE(server.requests.size(), 0);
    write(source, Bytes);
    server.handler = [](QTcpSocket *socket, const QByteArray &) { Server::reply(socket, metadata(Bytes + 'x'), 201); };
    client.upload(source, StreamUuid, "attachment.bin", done);
    QTRY_COMPARE(completions, 3); QVERIFY(!result.ok());
    server.handler = [](QTcpSocket *, const QByteArray &) {};
    client.upload(source, StreamUuid, "attachment.bin", done);
    QTRY_COMPARE(server.requests.size(), 2);
    api.clearSession();
    QTRY_COMPARE(completions, 4); QVERIFY(!result.ok()); QCOMPARE(result.status, 0);
    client.upload(source, StreamUuid, "attachment.bin", done);
    QTRY_COMPARE(completions, 5); QVERIFY(!result.ok()); QCOMPARE(server.requests.size(), 2);
}

void FileClientTest::noRedirectedBearerOrCookies()
{
    Server server, other;
    const auto redirect = "Location: " + other.origin().toEncoded() + "/collect\r\n";
    other.handler = [](QTcpSocket *socket, const QByteArray &) { Server::reply(socket, ""); };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    const auto source = directory.filePath("source.bin"); write(source, Bytes);
    const auto destination = directory.filePath("saved.bin");
    for (const auto &phase : {QString("metadata"), QString("binary"), QString("upload")}) {
        const auto requestsBefore = server.requests.size();
        server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
            if (phase == "binary" && !request.contains("/actions/download ")) Server::reply(socket, metadata(), 200, "Set-Cookie: session=forbidden\r\n");
            else Server::reply(socket, "", 302, redirect);
        };
        bool done = false; FileResult result;
        const auto completion = [&](const FileResult &r) { result = r; done = true; };
        if (phase == "upload") client.upload(source, StreamUuid, "attachment.bin", completion);
        else client.download(FileUuid, destination, completion);
        QTRY_VERIFY(done); QVERIFY(!result.ok()); QCOMPARE(result.status, 302);
        QCOMPARE(other.requests.size(), 0);
        QCOMPARE(server.requests.size() - requestsBefore, phase == "binary" ? 2 : 1);
    }
    for (const auto &request : server.requests) QVERIFY(header(request, "Cookie").isEmpty());
    // These narrow transport entrypoints cannot be used with arbitrary paths.
    QVERIFY(!api.startFileDownload("//host/collect"));
    QVERIFY(!api.startFileMetadata(FileUuid + "/../other"));
}

void FileClientTest::writeErrorsAndDeadline()
{
    Server server;
    server.handler = [](QTcpSocket *socket, const QByteArray &) { Server::reply(socket, metadata()); };
    ApiClient api(nullptr, true); authorize(api, server);
    FileTransferLimits limits; limits.timeoutMs = 100;
    FileClient client(&api, nullptr, limits);
    QTemporaryDir directory;
    bool done = false; FileResult result;
    client.download(FileUuid, directory.path(), [&](const FileResult &r) { result = r; done = true; });
    QTRY_VERIFY(done); QVERIFY(!result.ok());
    QCOMPARE(server.requests.size(), 1); // Failed staging never starts binary download.
    server.handler = [](QTcpSocket *, const QByteArray &) {};
    done = false;
    client.download(FileUuid, directory.filePath("saved.bin"), [&](const FileResult &r) { result = r; done = true; });
    QTRY_VERIFY(done); QVERIFY(!result.ok()); QVERIFY(result.error.contains("timed out"));
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
}

void FileClientTest::absentChecksumAndEmptyDownload()
{
    Server server;
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    for (const auto &bytes : {Bytes, QByteArray()}) {
        server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
            auto object = QJsonDocument::fromJson(metadata(bytes)).object(); object["hash"] = QJsonValue::Null;
            Server::reply(socket, request.contains("/actions/download ") ? bytes : QJsonDocument(object).toJson(QJsonDocument::Compact));
        };
        FileResult result; bool done = false;
        const auto destination = directory.filePath("saved.bin");
        client.download(FileUuid, destination, [&](const FileResult &r) { result = r; done = true; });
        QTRY_VERIFY(done); QVERIFY2(result.ok(), qPrintable(result.error)); QVERIFY(!result.checksumVerified);
        QCOMPARE(read(destination), bytes);
    }
}

void FileClientTest::typedUrns()
{
    FileMetadata metadata;
    metadata.uuid = FileUuid; metadata.name = "Привет C++ 100% ?&.png"; metadata.sizeBytes = 16;
    for (const auto &type : {QString("image/png"), QString("video/mp4"), QString("application/pdf")}) {
        metadata.contentType = type;
        const auto prefix = type.startsWith("image/") ? "image" : type.startsWith("video/") ? "video" : "file";
        const auto urn = metadata.urn();
        QVERIFY(urn.startsWith(QString("urn:%1:%2?").arg(prefix, FileUuid)));
        // The web client uses URLSearchParams, which reads an unescaped '+' as a space.
        QVERIFY(urn.contains("C%2B%2B"));
        const QUrlQuery query(urn.mid(urn.indexOf('?') + 1));
        QCOMPARE(query.queryItemValue("name", QUrl::FullyDecoded), metadata.name);
        QCOMPARE(query.queryItemValue("content_type", QUrl::FullyDecoded), type);
        QCOMPARE(query.queryItemValue("size"), "16");
    }
    metadata.uuid = "https://other-host";
    QVERIFY(metadata.urn().isEmpty());
}

void FileClientTest::streamingAcrossMultipleChunks()
{
    QByteArray bytes(257 * 1024 + 17, '\0');
    for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
    Server server;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!request.contains("/actions/download ")) { Server::reply(socket, metadata(bytes), request.startsWith("POST") ? 201 : 200); return; }
        // No Content-Length: streaming limit must also work on a close-delimited body.
        socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
        auto *pump = new QTimer(socket);
        connect(pump, &QTimer::timeout, socket, [socket, pump, bytes, offset = qsizetype(0)]() mutable {
            socket->write(bytes.mid(offset, 8192));
            offset += 8192;
            if (offset >= bytes.size()) { pump->stop(); socket->disconnectFromHost(); }
        });
        pump->start(0);
    };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    const auto source = directory.filePath("source.bin"); write(source, bytes);
    FileResult result; bool done = false;
    client.upload(source, StreamUuid, "attachment.bin", [&](const FileResult &r) { result = r; done = true; });
    QTRY_VERIFY(done); QVERIFY2(result.ok(), qPrintable(result.error));
    QVERIFY(server.requests.first().contains(bytes));
    const auto destination = directory.filePath("saved.bin");
    done = false;
    client.download(FileUuid, destination, [&](const FileResult &r) { result = r; done = true; });
    QTRY_VERIFY(done); QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(read(destination), bytes); QVERIFY(result.checksumVerified);
}

void FileClientTest::apiDestructionCancelsTransfers()
{
    Server server;
    QPointer<QTcpSocket> held;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!request.contains("/actions/download ")) Server::reply(socket, metadata());
        else held = socket;
    };
    auto api = std::make_unique<ApiClient>(nullptr, true); authorize(*api, server);
    auto client = std::make_unique<FileClient>(api.get());
    QTemporaryDir directory;
    int completions = 0;
    client->download(FileUuid, directory.filePath("saved.bin"), [&](const FileResult &result) {
        QVERIFY(!result.ok()); ++completions;
        client.reset(); // Completion may synchronously destroy its FileClient.
    });
    QTRY_VERIFY(held);
    api.reset();
    QCOMPARE(completions, 1);
    QVERIFY(!client);
    QTRY_VERIFY(!held || held->state() == QAbstractSocket::UnconnectedState);
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
}

void FileClientTest::realWriteFailurePreservesDestination()
{
#ifndef Q_OS_UNIX
    QSKIP("Real file-size resource limit is available on Unix; open/commit failures are tested elsewhere.");
#else
    const QByteArray bytes(128 * 1024, '\0');
    Server server;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.contains("/actions/download ") ? bytes : metadata(bytes));
    };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    QTemporaryDir directory;
    const auto destination = directory.filePath("saved.bin"); write(destination, "original");
    struct rlimit previous;
    QVERIFY(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    auto limited = previous; limited.rlim_cur = std::min<rlim_t>(4096, previous.rlim_max);
    const auto previousHandler = std::signal(SIGXFSZ, SIG_IGN);
    const bool installed = setrlimit(RLIMIT_FSIZE, &limited) == 0;
    FileResult result; bool done = false;
    if (installed) {
        client.download(FileUuid, destination, [&](const FileResult &r) { result = r; done = true; });
        QElapsedTimer deadline; deadline.start();
        while (!done && deadline.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        setrlimit(RLIMIT_FSIZE, &previous);
    }
    std::signal(SIGXFSZ, previousHandler);
    QVERIFY(installed); QVERIFY(done); QVERIFY(!result.ok());
    QVERIFY(result.error.contains("write") || result.error.contains("commit"));
    QCOMPARE(read(destination), "original");
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"saved.bin"});
#endif
}

void FileClientTest::memoryDownloadReturnsBytes_data()
{
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario : {"avatar-null-stream", "empty", "optional-hash",
                                "multiple-chunks-no-length", "exact-25-MiB"})
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
}

void FileClientTest::memoryDownloadReturnsBytes()
{
    QFETCH(QString, scenario);
    auto bytes = scenario == "empty" ? QByteArray{} : Bytes;
    if (scenario == "multiple-chunks-no-length") bytes = QByteArray(257 * 1024 + 17, 's');
    if (scenario == "exact-25-MiB") bytes = QByteArray(25 * 1024 * 1024, 'b');
    FileResult result;
    int completions = 0;
    Server server;
    auto object = QJsonDocument::fromJson(metadata(bytes)).object();
    object["stream_uuid"] = QJsonValue::Null; // Uploaded avatars have no stream binding.
    object["content_type"] = "image/png"; // Transport bytes only; not a decoder acceptance fixture.
    if (scenario == "optional-hash") object["hash"] = QJsonValue::Null;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!request.contains("/actions/download ")) {
            Server::reply(socket, QJsonDocument(object).toJson(QJsonDocument::Compact), 200,
                          "Set-Cookie: never-send=synthetic\r\n");
        } else if (scenario == "multiple-chunks-no-length") {
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
            auto *pump = new QTimer(socket);
            connect(pump, &QTimer::timeout, socket, [socket, pump, bytes, offset = qsizetype(0)]() mutable {
                socket->write(bytes.mid(offset, 8192));
                offset += 8192;
                if (offset >= bytes.size()) { pump->stop(); socket->disconnectFromHost(); }
            });
            pump->start(0); // Same real chunked-arrival fixture as the existing disk test.
        } else Server::reply(socket, bytes);
    };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    const auto operation = client.downloadBytes(FileUuid.toUpper(), [&](const FileResult &value) {
        result = value; ++completions;
    });
    QVERIFY(operation != 0);
    QTRY_COMPARE(completions, 1);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.status, 200);
    QCOMPARE(result.bytes, bytes);
    QVERIFY(result.path.isEmpty());
    QCOMPARE(result.metadata.uuid, FileUuid);
    QVERIFY(result.metadata.streamUuid.isEmpty());
    QCOMPARE(result.metadata.sizeBytes, bytes.size());
    QCOMPARE(result.checksumVerified, scenario != "optional-hash");
    QCOMPARE(FileClient::MemoryDownloadLimitBytes, qint64(25 * 1024 * 1024));
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(server.requests[0].startsWith("GET /api/workspace/v1/messenger/files/" + FileUuid.toUtf8() + " HTTP/1.1"));
    QVERIFY(server.requests[1].startsWith("GET /api/workspace/v1/messenger/files/" + FileUuid.toUtf8() + "/actions/download HTTP/1.1"));
    for (const auto &request : server.requests) {
        QCOMPARE(header(request, "Authorization"), "Bearer file-test-token");
        QCOMPARE(header(request, "Accept-Encoding"), "identity");
        QVERIFY(header(request, "Cookie").isEmpty());
    }
    client.cancel(operation); // A completed operation cannot complete twice.
    QCOMPARE(completions, 1);
}

void FileClientTest::memoryDownloadRejectsUnsafeTransfers_data()
{
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario : {"invalid-uuid", "unsigned", "declared-over-25-MiB",
             "wrong-uuid", "invalid-json", "metadata-403", "metadata-404",
             "metadata-redirect", "binary-403", "binary-404", "binary-redirect",
             "wrong-content-length", "stream-overflow", "truncated", "bad-hash"})
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
}

void FileClientTest::memoryDownloadRejectsUnsafeTransfers()
{
    QFETCH(QString, scenario);
    FileResult result;
    int completions = 0;
    Server server, other;
    auto object = QJsonDocument::fromJson(metadata()).object();
    object["stream_uuid"] = QJsonValue::Null;
    if (scenario == "declared-over-25-MiB") object["size_bytes"] = 25 * 1024 * 1024 + 1;
    if (scenario == "wrong-uuid") object["uuid"] = StreamUuid;
    const auto redirect = "Location: " + other.origin().toEncoded() + "/collect\r\n";
    other.handler = [](QTcpSocket *socket, const QByteArray &) { Server::reply(socket, ""); };
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!request.contains("/actions/download ")) {
            if (scenario == "metadata-403") { Server::reply(socket, "private failure body", 403); return; }
            if (scenario == "metadata-404") { Server::reply(socket, "private failure body", 404); return; }
            if (scenario == "metadata-redirect") { Server::reply(socket, "", 302, redirect); return; }
            Server::reply(socket, scenario == "invalid-json" ? QByteArray("not json")
                : QJsonDocument(object).toJson(QJsonDocument::Compact), 200,
                "Set-Cookie: never-send=synthetic\r\n");
            return;
        }
        if (scenario == "binary-403") { Server::reply(socket, "private failure body", 403); return; }
        if (scenario == "binary-404") { Server::reply(socket, "private failure body", 404); return; }
        if (scenario == "binary-redirect") { Server::reply(socket, "", 302, redirect); return; }
        if (scenario == "wrong-content-length") { Server::reply(socket, Bytes + 'x'); return; }
        if (scenario == "stream-overflow") {
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + Bytes + 'x');
            socket->disconnectFromHost(); return;
        }
        if (scenario == "truncated") {
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Bytes.size())
                          + "\r\nConnection: close\r\n\r\n" + Bytes.left(3));
            socket->disconnectFromHost(); return;
        }
        Server::reply(socket, scenario == "bad-hash" ? QByteArray(Bytes.size(), 'x') : Bytes);
    };
    ApiClient api(nullptr, true); authorize(api, server);
    if (scenario == "unsigned") api.clearSession();
    FileClient client(&api);
    const auto operation = client.downloadBytes(scenario == "invalid-uuid" ? FileUuid + "/../other" : FileUuid,
        [&](const FileResult &value) { result = value; ++completions; });
    QVERIFY(operation != 0);
    QTRY_COMPARE(completions, 1);
    QVERIFY(!result.ok()); QVERIFY(!result.error.isEmpty());
    QVERIFY(!result.error.contains("private failure body"));
    QVERIFY(result.bytes.isEmpty()); QVERIFY(result.path.isEmpty());
    QVERIFY(!result.checksumVerified);
    const bool local = scenario == "invalid-uuid" || scenario == "unsigned";
    const bool binary = scenario.startsWith("binary-") || scenario == "wrong-content-length"
        || scenario == "stream-overflow" || scenario == "truncated" || scenario == "bad-hash";
    QCOMPARE(server.requests.size(), local ? 0 : binary ? 2 : 1);
    QCOMPARE(other.requests.size(), 0);
    if (scenario.endsWith("403")) QCOMPARE(result.status, 403);
    if (scenario.endsWith("404")) QCOMPARE(result.status, 404);
    if (scenario.endsWith("redirect")) QCOMPARE(result.status, 302);
    for (const auto &request : server.requests) {
        QCOMPARE(header(request, "Authorization"), "Bearer file-test-token");
        QVERIFY(header(request, "Cookie").isEmpty());
    }
    client.cancel(operation);
    QCOMPARE(completions, 1);
}

void FileClientTest::memoryDownloadLifetime_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<bool>("metadataPhase");
    QTest::newRow("cancel-metadata") << QString("cancel") << true;
    QTest::newRow("cancel-binary") << QString("cancel") << false;
    QTest::newRow("generation-metadata") << QString("generation") << true;
    QTest::newRow("generation-binary") << QString("generation") << false;
    QTest::newRow("destroy-client") << QString("client") << false;
    QTest::newRow("destroy-api") << QString("api") << false;
    QTest::newRow("deadline") << QString("deadline") << false;
}

void FileClientTest::memoryDownloadLifetime()
{
    QFETCH(QString, action); QFETCH(bool, metadataPhase);
    FileResult result;
    int completions = 0;
    QPointer<QTcpSocket> held;
    Server server;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (!metadataPhase && !request.contains("/actions/download ")) {
            Server::reply(socket, metadata()); return;
        }
        held = socket;
        if (!metadataPhase)
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Bytes.size())
                          + "\r\n\r\n" + Bytes.left(3));
    };
    auto api = std::make_unique<ApiClient>(nullptr, true); authorize(*api, server);
    FileTransferLimits limits;
    if (action == "deadline") limits.timeoutMs = 100; // Existing FileClient deadline fixture value.
    auto client = std::make_unique<FileClient>(api.get(), nullptr, limits);
    const auto operation = client->downloadBytes(FileUuid, [&](const FileResult &value) {
        result = value; ++completions;
    });
    QTRY_COMPARE(server.requests.size(), metadataPhase ? 1 : 2); // Wire acknowledgment before mutation.
    QVERIFY(held); QVERIFY(held->state() == QAbstractSocket::ConnectedState);
    if (action == "cancel") { client->cancel(operation); client->cancel(operation); }
    else if (action == "generation") api->setAccessToken("replacement-synthetic-token");
    else if (action == "client") client.reset();
    else if (action == "api") api.reset();
    if (action != "client") {
        QTRY_COMPARE(completions, 1);
        QVERIFY(!result.ok()); QCOMPARE(result.status, 0);
        QVERIFY(result.bytes.isEmpty()); QVERIFY(result.path.isEmpty());
    } else QCOMPARE(completions, 0); // FileClient destruction intentionally suppresses callbacks.
    QTRY_VERIFY(!held || held->state() == QAbstractSocket::UnconnectedState);
    QCoreApplication::processEvents();
    QCOMPARE(completions, action == "client" ? 0 : 1);
    QCOMPARE(server.requests.size(), metadataPhase ? 1 : 2);
}

void FileClientTest::memoryCancellationDoesNotCancelAnotherOperation()
{
    int first = 0, second = 0;
    FileResult secondResult;
    QPointer<QTcpSocket> abandoned;
    Server server;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (server.requests.size() == 1) { abandoned = socket; return; }
        Server::reply(socket, request.contains("/actions/download ") ? Bytes : metadata());
    };
    ApiClient api(nullptr, true); authorize(api, server);
    FileClient client(&api);
    const auto cancelled = client.downloadBytes(FileUuid, [&](const FileResult &result) {
        QVERIFY(!result.ok()); QVERIFY(result.bytes.isEmpty()); ++first;
    });
    QTRY_COMPARE(server.requests.size(), 1); QVERIFY(abandoned);
    client.downloadBytes(FileUuid, [&](const FileResult &result) { secondResult = result; ++second; });
    client.cancel(cancelled);
    QTRY_COMPARE(first, 1); QTRY_COMPARE(second, 1);
    QVERIFY2(secondResult.ok(), qPrintable(secondResult.error));
    QCOMPARE(secondResult.bytes, Bytes); QVERIFY(secondResult.path.isEmpty());
    QCOMPARE(server.requests.size(), 3);
    QTRY_VERIFY(!abandoned || abandoned->state() == QAbstractSocket::UnconnectedState);
}

void FileClientTest::memoryCompletionMayDestroyItsClient()
{
    int completions = 0;
    QByteArray received;
    Server server;
    server.handler = [](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.contains("/actions/download ") ? Bytes : metadata());
    };
    ApiClient api(nullptr, true); authorize(api, server);
    auto client = std::make_unique<FileClient>(&api);
    client->downloadBytes(FileUuid, [&](const FileResult &result) {
        if (result.ok()) received = result.bytes;
        ++completions;
        client.reset();
    });
    QTRY_COMPARE(completions, 1);
    QVERIFY(!client); QCOMPARE(received, Bytes);
    QCOMPARE(server.requests.size(), 2);
    QCoreApplication::processEvents();
    QCOMPARE(completions, 1);
}

QTEST_GUILESS_MAIN(FileClientTest)
#include "file-client-test.moc"
