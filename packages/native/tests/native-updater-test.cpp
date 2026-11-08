#include "native-updater.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>
#include <utility>

using namespace workspace;

namespace {
const QByteArray Archive("synthetic tar bytes\0not an installer", 36);
QByteArray sha(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
QByteArray header(const QByteArray &request, const QByteArray &name)
{
    for (const auto &line : request.left(request.indexOf("\r\n\r\n")).split('\n'))
        if (line.toLower().startsWith(name.toLower() + ':')) return line.mid(line.indexOf(':') + 1).trimmed();
    return {};
}
class Server final : public QObject {
public:
    Server()
    {
        listening = tcp.listen(QHostAddress::LocalHost);
        connect(&tcp, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = tcp.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto &buffer = buffers[socket];
                    buffer += socket->readAll();
                    if (!buffer.contains("\r\n\r\n")) return;
                    const auto request = buffer;
                    buffers.remove(socket);
                    requests.append(request);
                    if (handler) handler(socket, request);
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }
    ~Server() override
    {
        for (auto *socket : tcp.findChildren<QTcpSocket *>()) {
            disconnect(socket, nullptr, this, nullptr);
            socket->abort();
        }
    }
    QUrl url(const QString &path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(tcp.serverPort()).arg(path)); }
    static void reply(QTcpSocket *socket, const QByteArray &body, int status = 200, const QByteArray &extra = {})
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Synthetic\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
        socket->disconnectFromHost();
    }
    QJsonObject release(const QString &tag = QStringLiteral("v1.2.4")) const
    {
        return {{"tag_name", tag}, {"draft", false}, {"prerelease", false},
            {"assets", QJsonArray{QJsonObject{{"name", "Exordos-Workspace-Native-1.2.4-linux-x86_64.tar.gz"},
                {"size", archive.size()}, {"digest", "sha256:" + QString::fromLatin1(sha(archive))},
                {"browser_download_url", url(QStringLiteral("/archive")).toString()}}}}};
    }
    QTcpServer tcp;
    bool listening = false;
    QHash<QTcpSocket *, QByteArray> buffers;
    QList<QByteArray> requests;
    std::function<void(QTcpSocket *, const QByteArray &)> handler;
    QByteArray archive = Archive;
};
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
struct Environment final {
    explicit Environment(QByteArray name, const QByteArray &value)
        : name(std::move(name)), value(qgetenv(this->name.constData())), existed(qEnvironmentVariableIsSet(this->name.constData()))
    { qputenv(this->name.constData(), value); }
    ~Environment() { if (existed) qputenv(name.constData(), value); else qunsetenv(name.constData()); }
    QByteArray name;
    QByteArray value;
    bool existed;
};
QByteArray package(const QString &mode)
{
    // Synthetic contract installer, not Root's distribution/install acceptance.
    QTemporaryDir directory;
    if (!directory.isValid()) return {};
    const auto name = mode == QStringLiteral("wrong-root") ? QStringLiteral("wrong-root")
        : QStringLiteral("workspace-native-1.2.4-linux-x86_64");
    const auto prefix = directory.filePath(name);
    if (!QDir().mkpath(prefix + QStringLiteral("/bin"))) return {};
    if (mode != QStringLiteral("missing-version") && !write(prefix + QStringLiteral("/VERSION"),
        mode == QStringLiteral("wrong-version") ? QByteArray("1.2.5\n") : QByteArray("1.2.4\n"))) return {};
    const auto binary = prefix + QStringLiteral("/bin/workspace-native");
    if (!write(binary, "#!/bin/sh\nexit 0\n") || !QFile::setPermissions(binary,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return {};
    QByteArray script = R"SH(#!/bin/bash
set -eu
test "$#" = 1 && test "$1" = --no-launch
src="$(cd -- "$(dirname -- "$0")" && pwd)"
root="${XDG_DATA_HOME:-$HOME/.local/share}/workspace-native-install"
mkdir -p "$root/versions/1.2.4"
cp -a "$src/VERSION" "$root/versions/1.2.4/VERSION"
cp -a "$src/bin" "$root/versions/1.2.4/bin"
ln -s versions/1.2.4 "$root/.current-new"
mv -Tf "$root/.current-new" "$root/current"
)SH";
    if (mode == QStringLiteral("installer-error")) script = "#!/bin/bash\nexit 7\n";
    if (mode == QStringLiteral("held-installer")) script = R"SH(#!/bin/bash
set -eu
echo ready > "$HOME/updater-installer-started"
sleep 60 &
wait
)SH";
    if (mode == QStringLiteral("held-cleanup-installer")) script = R"SH(#!/bin/bash
set -eu
active="$HOME/updater-installer-active"
mkdir -- "$active"
cleanup() { rmdir -- "$active"; }
trap cleanup EXIT
trap 'exit 143' TERM
sleep 60 &
child=$!
printf '%s\n' "$child" > "$HOME/updater-installer-child"
printf ready > "$HOME/updater-installer-cleanup-ready"
wait "$child"
)SH";
    if (!write(prefix + QStringLiteral("/install.sh"), script)) return {};
    if (mode == QStringLiteral("absolute-link") && !QFile::link(QStringLiteral("/tmp"), prefix + QStringLiteral("/escape"))) return {};
    if (mode == QStringLiteral("parent-link") && !QFile::link(QStringLiteral("../outside"), prefix + QStringLiteral("/escape"))) return {};
    const auto output = directory.filePath(QStringLiteral("archive.tar.gz"));
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment(); env.remove(QStringLiteral("TAR_OPTIONS")); env.remove(QStringLiteral("GZIP"));
    process.setProcessEnvironment(env);
    process.start(QStringLiteral("/usr/bin/tar"), {QStringLiteral("-czf"), output,
        QStringLiteral("-C"), directory.path(), name});
    if (!process.waitForFinished(2000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {};
    return read(output);
}
}

class NativeUpdaterTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(QRegularExpression(QStringLiteral(".*"))); }
    void explicitCheckAndVerifiedStreamingDownload();
    void plainNumericReleaseTagAndVerifiedStreamingDownload_data();
    void plainNumericReleaseTagAndVerifiedStreamingDownload();
    void nonNewerRelease_data();
    void nonNewerRelease();
    void invalidMetadata_data();
    void invalidMetadata();
    void rejectedDownload_data();
    void rejectedDownload();
    void sameOriginRedirectCarriesNoCookies();
    void cancellationAndDestructionRemoveOnlyOwnedPartialFiles();
    void outwardNotificationMayCancelOrDelete_data();
    void outwardNotificationMayCancelOrDelete();
    void invalidEndpointDoesNotMakeARequest();
    void installAndRestartUseTheManagedContract();
    void rejectedInstall_data();
    void rejectedInstall();
    void cancellationTerminatesTheInstallerAndRemovesStaging();
    void cancellationAllowsTheInstallerToRemoveItsOwnActiveLock();
};

void NativeUpdaterTest::explicitCheckAndVerifiedStreamingDownload()
{
    Server server;
    QVERIFY(server.listening);
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    QPointer<QTcpSocket> binary;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (request.startsWith("GET /latest ")) Server::reply(socket, json(server.release()), 200, "Set-Cookie: secret=ignored\r\n");
        else {
            binary = socket;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Archive.size())
                + "\r\nConnection: close\r\n\r\n" + Archive.left(5));
        }
    };
    QString verified;
    {
        NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
        QCOMPARE(updater.state(), QStringLiteral("idle"));
        QCOMPARE(server.requests.size(), 0); // Construction/getters never request.
        updater.check(); updater.check();
        QTRY_COMPARE(updater.state(), QStringLiteral("available"));
        QCOMPARE(updater.availableVersion(), QStringLiteral("1.2.4"));
        QCOMPARE(server.requests.size(), 1);
        updater.download(); updater.download();
        QTRY_COMPARE(server.requests.size(), 2);
        QTRY_VERIFY(updater.progress() > 0);
        QVERIFY(updater.progress() < 1);
        QVERIFY(updater.downloadedPath().isEmpty());
        QVERIFY(binary);
        binary->write(Archive.mid(5)); binary->disconnectFromHost();
        QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
        QCOMPARE(updater.progress(), qreal(1));
        verified = updater.downloadedPath();
        QVERIFY(QFileInfo(verified).isFile());
        QCOMPARE(read(verified), Archive);
        for (const auto &request : server.requests) {
            QVERIFY(header(request, "Authorization").isEmpty());
            QVERIFY(header(request, "Cookie").isEmpty());
            QCOMPARE(header(request, "Accept-Encoding"), QByteArray("identity"));
        }
        updater.check(); // A scheduled check cannot discard a verified pending update.
        QCOMPARE(updater.state(), QStringLiteral("downloaded"));
        QCOMPARE(server.requests.size(), 2);
    }
    QVERIFY(!QFileInfo::exists(verified));
}

void NativeUpdaterTest::plainNumericReleaseTagAndVerifiedStreamingDownload_data()
{
    QTest::addColumn<QString>("currentVersion");
    QTest::addColumn<QString>("releaseVersion");
    QTest::newRow("existing-patch") << QStringLiteral("1.2.3") << QStringLiteral("1.2.4");
    QTest::newRow("preview-bugfix") << QStringLiteral("0.1.6") << QStringLiteral("0.1.7");
    QTest::newRow("preview-feature") << QStringLiteral("0.1.6") << QStringLiteral("0.2.0");
    QTest::newRow("feature-bugfix") << QStringLiteral("0.2.0") << QStringLiteral("0.2.1");
    QTest::newRow("two-digit-patch") << QStringLiteral("0.2.9") << QStringLiteral("0.2.10");
    QTest::newRow("two-digit-minor") << QStringLiteral("0.9.9") << QStringLiteral("0.10.0");
    QTest::newRow("major-interface-change") << QStringLiteral("0.99.99") << QStringLiteral("1.0.0");
}

void NativeUpdaterTest::plainNumericReleaseTagAndVerifiedStreamingDownload()
{
    QFETCH(QString, currentVersion);
    QFETCH(QString, releaseVersion);
    Server server;
    QVERIFY(server.listening);
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    auto release = server.release(releaseVersion);
    auto asset = release.value(QStringLiteral("assets")).toArray().first().toObject();
    asset.insert(QStringLiteral("name"),
        QStringLiteral("Exordos-Workspace-Native-%1-linux-x86_64.tar.gz").arg(releaseVersion));
    release.insert(QStringLiteral("assets"), QJsonArray{asset});
    QCOMPARE(release.value(QStringLiteral("tag_name")).toString(), releaseVersion);
    QPointer<QTcpSocket> binary;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (request.startsWith("GET /latest ")) Server::reply(socket, json(release));
        else {
            binary = socket;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Archive.size())
                + "\r\nConnection: close\r\n\r\n" + Archive.left(5));
        }
    };
    NativeUpdater updater(server.url(QStringLiteral("/latest")), currentVersion, cache.path());
    updater.check();
    QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    QCOMPARE(updater.availableVersion(), releaseVersion);
    QVERIFY(updater.error().isEmpty());
    QCOMPARE(server.requests.size(), 1);
    updater.download();
    QTRY_COMPARE(server.requests.size(), 2);
    QTRY_VERIFY(updater.progress() > 0);
    QVERIFY(updater.progress() < 1);
    QVERIFY(updater.downloadedPath().isEmpty());
    QVERIFY(binary);
    binary->write(Archive.mid(5));
    binary->disconnectFromHost();
    QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
    QCOMPARE(updater.progress(), qreal(1));
    QVERIFY(QFileInfo(updater.downloadedPath()).isFile());
    QCOMPARE(read(updater.downloadedPath()), Archive);
    QVERIFY(updater.error().isEmpty());
    QCOMPARE(server.requests.size(), 2);
    for (const auto &request : server.requests) {
        QVERIFY(header(request, "Authorization").isEmpty());
        QVERIFY(header(request, "Cookie").isEmpty());
    }
}

void NativeUpdaterTest::nonNewerRelease_data()
{
    QTest::addColumn<QString>("tag");
    QTest::newRow("same") << QStringLiteral("v1.2.10");
    QTest::newRow("older") << QStringLiteral("1.1.9");
    QTest::newRow("numeric-not-lexical") << QStringLiteral("1.2.9");
}
void NativeUpdaterTest::nonNewerRelease()
{
    QFETCH(QString, tag);
    Server server;
    QVERIFY(server.listening);
    server.handler = [&](QTcpSocket *socket, const QByteArray &) { Server::reply(socket, json(server.release(tag))); };
    QTemporaryDir cache;
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.10"), cache.path());
    updater.check();
    QTRY_COMPARE(updater.state(), QStringLiteral("current"));
    QVERIFY(updater.availableVersion().isEmpty());
    updater.download(); updater.install(); updater.restart();
    QCOMPARE(server.requests.size(), 1);
}

void NativeUpdaterTest::invalidMetadata_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"draft", "prerelease", "leading-zero", "suffix", "no-digest", "bad-digest",
             "duplicate", "no-match", "fractional-size", "oversize", "foreign-url", "invalid-json", "declared-4MiB"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}
void NativeUpdaterTest::invalidMetadata()
{
    QFETCH(QString, mode);
    Server server;
    QVERIFY(server.listening);
    auto release = server.release();
    auto assets = release.value("assets").toArray();
    auto asset = assets.first().toObject();
    if (mode == "draft") release["draft"] = true;
    if (mode == "prerelease") release["prerelease"] = true;
    if (mode == "leading-zero") release["tag_name"] = "v01.2.4";
    if (mode == "suffix") release["tag_name"] = "v1.2.4-beta.1";
    if (mode == "no-digest") asset.remove("digest");
    if (mode == "bad-digest") asset["digest"] = "sha256:no";
    if (mode == "no-match") asset["name"] = "Exordos-Workspace-Native-1.2.4-linux-arm64.tar.gz";
    if (mode == "fractional-size") asset["size"] = 1.5;
    if (mode == "oversize") asset["size"] = double(NativeUpdater::MaximumArchiveBytes) + 1;
    if (mode == "foreign-url") asset["browser_download_url"] = "http://192.0.2.1/archive";
    assets[0] = asset;
    if (mode == "duplicate") assets.append(asset);
    release["assets"] = assets;
    server.handler = [&](QTcpSocket *socket, const QByteArray &) {
        if (mode == "declared-4MiB") {
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 4194305\r\n\r\n");
            return;
        }
        Server::reply(socket, mode == "invalid-json" ? QByteArray("invalid") : json(release));
    };
    QTemporaryDir cache;
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
    updater.check();
    QTRY_COMPARE(updater.state(), QStringLiteral("error"));
    QVERIFY(!updater.error().isEmpty());
    QVERIFY(updater.downloadedPath().isEmpty());
    updater.download();
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(QDir(cache.path()).entryList(QDir::Files).isEmpty());
}

void NativeUpdaterTest::rejectedDownload_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"hash", "truncated", "chunk-overflow", "declared-512MiB", "redirect-origin", "redirect-loop", "http403"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}
void NativeUpdaterTest::rejectedDownload()
{
    QFETCH(QString, mode);
    Server server;
    QVERIFY(server.listening);
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (request.startsWith("GET /latest ")) { Server::reply(socket, json(server.release())); return; }
        if (mode == "hash") { Server::reply(socket, QByteArray(Archive.size(), 'x')); return; }
        if (mode == "truncated") {
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Archive.size()) + "\r\n\r\nshort");
            socket->disconnectFromHost(); return;
        }
        if (mode == "chunk-overflow") {
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + Archive + 'x');
            socket->disconnectFromHost(); return;
        }
        if (mode == "declared-512MiB") { socket->write("HTTP/1.1 200 OK\r\nContent-Length: 536870913\r\n\r\n"); return; }
        if (mode == "redirect-origin") { Server::reply(socket, {}, 302, "Location: http://192.0.2.1/forbidden\r\n"); return; }
        if (mode == "redirect-loop") { Server::reply(socket, {}, 302, "Location: /archive\r\n"); return; }
        Server::reply(socket, "do-not-display-this-response", 403);
    };
    QTemporaryDir cache;
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
    updater.check();
    QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    updater.download();
    QTRY_COMPARE(updater.state(), QStringLiteral("error"));
    QVERIFY(updater.downloadedPath().isEmpty());
    QVERIFY(!updater.error().contains(QStringLiteral("do-not-display")));
    QVERIFY(QDir(cache.path()).entryList(QDir::Files).isEmpty());
    QCOMPARE(server.requests.size(), mode == "redirect-loop" ? 7 : 2);
}

void NativeUpdaterTest::sameOriginRedirectCarriesNoCookies()
{
    Server server;
    QVERIFY(server.listening);
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (request.startsWith("GET /latest ")) Server::reply(socket, json(server.release()));
        else if (request.startsWith("GET /archive ")) Server::reply(socket, {}, 302, "Location: /final\r\nSet-Cookie: token=must-not-follow\r\n");
        else Server::reply(socket, Archive);
    };
    QTemporaryDir cache;
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
    updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
    QCOMPARE(server.requests.size(), 3);
    for (const auto &request : server.requests) {
        QVERIFY(header(request, "Cookie").isEmpty());
        QVERIFY(header(request, "Authorization").isEmpty());
    }
}

void NativeUpdaterTest::cancellationAndDestructionRemoveOnlyOwnedPartialFiles()
{
    Server server;
    QVERIFY(server.listening);
    QPointer<QTcpSocket> held;
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (request.startsWith("GET /latest ")) Server::reply(socket, json(server.release()));
        else {
            held = socket;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Archive.size()) + "\r\n\r\nfirst");
        }
    };
    QTemporaryDir cache;
    const auto foreign = cache.filePath(QStringLiteral("unrelated.txt"));
    QFile file(foreign); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("keep"), qint64(4)); file.close();
    auto updater = std::make_unique<NativeUpdater>(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
    updater->check(); QTRY_COMPARE(updater->state(), QStringLiteral("available"));
    updater->download(); QTRY_COMPARE(server.requests.size(), 2); QTRY_VERIFY(updater->progress() > 0);
    QVERIFY(held);
    updater->cancel();
    QCOMPARE(updater->state(), QStringLiteral("idle"));
    QCOMPARE(QDir(cache.path()).entryList(QDir::Files), QStringList{QStringLiteral("unrelated.txt")});
    updater->check(); QTRY_COMPARE(updater->state(), QStringLiteral("available"));
    updater->download(); QTRY_COMPARE(server.requests.size(), 4); QTRY_VERIFY(updater->progress() > 0);
    updater.reset();
    QCOMPARE(read(foreign), QByteArray("keep"));
    QCOMPARE(QDir(cache.path()).entryList(QDir::Files), QStringList{QStringLiteral("unrelated.txt")});
}

void NativeUpdaterTest::outwardNotificationMayCancelOrDelete_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<bool>("remove");
    for (const auto *phase : {"checking", "available", "downloading", "progress", "downloaded"}) {
        QTest::newRow((QByteArray(phase) + "-delete").constData()) << QString::fromLatin1(phase) << true;
        QTest::newRow((QByteArray(phase) + "-cancel").constData()) << QString::fromLatin1(phase) << false;
    }
}
void NativeUpdaterTest::outwardNotificationMayCancelOrDelete()
{
    QFETCH(QString, phase);
    QFETCH(bool, remove);
    Server server;
    QVERIFY(server.listening);
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.startsWith("GET /latest ") ? json(server.release()) : Archive);
    };
    QTemporaryDir cache;
    auto updater = std::make_unique<NativeUpdater>(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
    bool triggered = false;
    connect(updater.get(), &NativeUpdater::changed, this, [&] {
        if (triggered || !updater) return;
        const bool match = phase == QStringLiteral("progress")
            ? updater->state() == QStringLiteral("downloading") && updater->progress() > 0
            : updater->state() == phase;
        if (!match) return;
        triggered = true;
        if (remove) updater.reset(); else updater->cancel();
    });
    updater->check();
    if (phase != QStringLiteral("checking") && phase != QStringLiteral("available")) {
        QTRY_VERIFY(updater && updater->state() == QStringLiteral("available"));
        updater->download();
    }
    QTRY_VERIFY(triggered);
    if (remove) QVERIFY(!updater); else QCOMPARE(updater->state(), QStringLiteral("idle"));
    QVERIFY(QDir(cache.path()).entryList(QDir::Files).isEmpty());
}

void NativeUpdaterTest::invalidEndpointDoesNotMakeARequest()
{
    QTemporaryDir cache;
    NativeUpdater updater(QUrl(QStringLiteral("http://192.0.2.1/latest")), QStringLiteral("1.2.3"), cache.path());
    updater.check();
    QCOMPARE(updater.state(), QStringLiteral("error"));
    QVERIFY(updater.findChildren<QNetworkReply *>().isEmpty());
}

void NativeUpdaterTest::installAndRestartUseTheManagedContract()
{
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    Environment home("HOME", sandbox.path().toUtf8());
    Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    Server server;
    QVERIFY(server.listening);
    server.archive = package(QStringLiteral("valid"));
    QVERIFY(!server.archive.isEmpty());
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.startsWith("GET /latest ") ? json(server.release()) : server.archive);
    };
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), sandbox.filePath(QStringLiteral("cache")));
    QSignalSpy restart(&updater, &NativeUpdater::restartRequested);
    updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
    QCOMPARE(restart.size(), 0);
    updater.install(); updater.install();
    QTRY_VERIFY(updater.state() == QStringLiteral("ready") || updater.state() == QStringLiteral("error"));
    QCOMPARE(updater.state(), QStringLiteral("ready"));
    QVERIFY(updater.downloadedPath().isEmpty());
    QVERIFY(QDir(sandbox.filePath(QStringLiteral("cache"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    QCOMPARE(restart.size(), 0); // Installation never exits/launches the app itself.
    updater.restart();
    QCOMPARE(restart.size(), 1);
    QCOMPARE(restart.first().first().toString(), sandbox.filePath(QStringLiteral("data/workspace-native-install/current/bin/workspace-native")));
    QCOMPARE(server.requests.size(), 2);
}

void NativeUpdaterTest::rejectedInstall_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"wrong-root", "missing-version", "wrong-version", "absolute-link", "parent-link", "installer-error"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}
void NativeUpdaterTest::rejectedInstall()
{
    QFETCH(QString, mode);
    QTemporaryDir sandbox;
    Environment home("HOME", sandbox.path().toUtf8());
    Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    Server server;
    QVERIFY(server.listening);
    server.archive = package(mode);
    QVERIFY(!server.archive.isEmpty());
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.startsWith("GET /latest ") ? json(server.release()) : server.archive);
    };
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), sandbox.filePath(QStringLiteral("cache")));
    QSignalSpy restart(&updater, &NativeUpdater::restartRequested);
    updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
    updater.install(); QTRY_COMPARE(updater.state(), QStringLiteral("error"));
    updater.restart(); QCOMPARE(restart.size(), 0);
    QVERIFY(!QFileInfo::exists(sandbox.filePath(QStringLiteral("data/workspace-native-install/current"))));
    QVERIFY(QDir(sandbox.filePath(QStringLiteral("cache"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

void NativeUpdaterTest::cancellationTerminatesTheInstallerAndRemovesStaging()
{
    QTemporaryDir sandbox;
    Environment home("HOME", sandbox.path().toUtf8());
    Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    Server server;
    QVERIFY(server.listening);
    server.archive = package(QStringLiteral("held-installer"));
    QVERIFY(!server.archive.isEmpty());
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.startsWith("GET /latest ") ? json(server.release()) : server.archive);
    };
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), sandbox.filePath(QStringLiteral("cache")));
    updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
    updater.install();
    QTRY_VERIFY(QFileInfo::exists(sandbox.filePath(QStringLiteral("updater-installer-started"))));
    QProcess *active = nullptr;
    for (auto *process : updater.findChildren<QProcess *>()) if (process->state() == QProcess::Running) active = process;
    QVERIFY(active);
    QPointer<QProcess> observed(active);
    updater.cancel();
    QCOMPARE(updater.state(), QStringLiteral("idle"));
    QVERIFY(!observed || observed->state() == QProcess::NotRunning);
    QVERIFY(QDir(sandbox.filePath(QStringLiteral("cache"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

void NativeUpdaterTest::cancellationAllowsTheInstallerToRemoveItsOwnActiveLock()
{
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    Environment home("HOME", sandbox.path().toUtf8());
    Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    Server server;
    QVERIFY(server.listening);
    server.archive = package(QStringLiteral("held-cleanup-installer"));
    QVERIFY(!server.archive.isEmpty());
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.startsWith("GET /latest ") ? json(server.release()) : server.archive);
    };
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), sandbox.filePath(QStringLiteral("cache")));
    QSignalSpy restart(&updater, &NativeUpdater::restartRequested);
    updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
    updater.install();
    const auto ready = sandbox.filePath(QStringLiteral("updater-installer-cleanup-ready"));
    const auto activeLock = sandbox.filePath(QStringLiteral("updater-installer-active"));
    QTRY_VERIFY(QFileInfo::exists(ready));
    QCOMPARE(read(ready), QByteArray("ready"));
    QVERIFY(QFileInfo(activeLock).isDir());
    bool validChild = false;
    const auto childPid = read(sandbox.filePath(QStringLiteral("updater-installer-child"))).trimmed().toLongLong(&validChild);
    QVERIFY(validChild && childPid > 0);
    QProcess *installer = nullptr;
    for (auto *process : updater.findChildren<QProcess *>())
        if (process->state() == QProcess::Running) installer = process;
    QVERIFY(installer);
    QPointer<QProcess> observed(installer);
    QCOMPARE(updater.state(), QStringLiteral("installing"));
    QCOMPARE(server.requests.size(), 2);
    updater.cancel();
    QCOMPARE(updater.state(), QStringLiteral("idle"));
    QVERIFY(!observed || observed->state() == QProcess::NotRunning);
    // First intended old-product failure: SIGKILL bypasses the real shell EXIT
    // trap. This lock is outside staging, so updater directory cleanup cannot
    // manufacture a pass by removing it on the installer's behalf.
    QVERIFY(!QFileInfo::exists(activeLock));
    QCOMPARE(restart.size(), 0);
    QVERIFY(!QFileInfo::exists(sandbox.filePath(QStringLiteral("data/workspace-native-install/current"))));
    QVERIFY(QDir(sandbox.filePath(QStringLiteral("cache"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

QTEST_GUILESS_MAIN(NativeUpdaterTest)
#include "native-updater-test.moc"
