#include "native-updater.h"
#include "native-updater-taskbar.h"

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
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
bool installedFixture(const QTemporaryDir &directory)
{
    const auto root = directory.filePath(QStringLiteral("data/workspace-native-install"));
    const auto target = root + QStringLiteral("/versions/1.2.4");
    const auto binary = target + QStringLiteral("/bin/workspace-native");
    return QDir().mkpath(target + QStringLiteral("/bin"))
        && write(target + QStringLiteral("/VERSION"), "1.2.4\n")
        && write(binary, "#!/bin/sh\nexit 0\n")
        && QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)
        && QFile::link(target, root + QStringLiteral("/current"));
}
struct Environment final {
    explicit Environment(QByteArray name, const QByteArray &value)
        : name(std::move(name)), value(qgetenv(this->name.constData())), existed(qEnvironmentVariableIsSet(this->name.constData()))
    { qputenv(this->name.constData(), value); }
    ~Environment()
    {
        if (existed) qputenv(name.constData(), value);
        else qunsetenv(name.constData());
    }
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

// A fake desktop launcher: owns the well-known com.canonical.Unity.LauncherEntry
// bus name that real launchers (Unity7, GNOME Dash-to-Dock, Plank, Latte) own
// and records every Update(desktop-entry, properties) signal delivered to it.
class LauncherEntryRecorder final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.Unity.LauncherEntry")
public:
    struct Call { QString desktopEntry; QVariantMap properties; };
    QList<Call> calls;
public slots:
    void Update(const QString &desktopEntry, const QVariantMap &properties)
    { calls.append(Call { desktopEntry, properties }); }
};

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
    void postInstallConfirmationPairing_data();
    void postInstallConfirmationPairing();
    void alreadyInstalledUpdate_data();
    void alreadyInstalledUpdate();
    void restartAfterVerifiedReuseRecordsConfirmation();
    void invalidExistingInstallation_data();
    void invalidExistingInstallation();
    void rejectedInstall_data();
    void rejectedInstall();
    void cancellationTerminatesTheInstallerAndRemovesStaging();
    void cancellationAllowsTheInstallerToRemoveItsOwnActiveLock();
    void downloadProgressMirrorsTheTaskbarHint_data();
    void downloadProgressMirrorsTheTaskbarHint();
    void downloadProgressReachesTheLauncherEntryChannel_data();
    void downloadProgressReachesTheLauncherEntryChannel();
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

void NativeUpdaterTest::alreadyInstalledUpdate_data()
{
    QTest::addColumn<bool>("installedBeforeCheck");
    QTest::newRow("retained-old-copy") << true;
    QTest::newRow("installed-during-download") << false;
}

void NativeUpdaterTest::alreadyInstalledUpdate()
{
    QFETCH(bool, installedBeforeCheck);
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    Server server;
    QVERIFY(server.listening);
    // Any attempt to apply this archive fails; the already selected immutable
    // installation must be reused without executing a second installer.
    server.archive = package(QStringLiteral("installer-error"));
    QVERIFY(!server.archive.isEmpty());
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.startsWith("GET /latest ") ? json(server.release()) : server.archive);
    };
    if (installedBeforeCheck) QVERIFY(installedFixture(sandbox));
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), sandbox.filePath(QStringLiteral("cache")));
    QSignalSpy restart(&updater, &NativeUpdater::restartRequested);
    updater.check();
    QTRY_VERIFY(updater.state() != QStringLiteral("checking"));
    if (!installedBeforeCheck) {
        QCOMPARE(updater.state(), QStringLiteral("available"));
        updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("downloaded"));
        QVERIFY(installedFixture(sandbox));
        updater.install();
        QTRY_VERIFY(updater.state() != QStringLiteral("installing"));
    }
    QCOMPARE(updater.state(), QStringLiteral("ready"));
    QCOMPARE(updater.availableVersion(), QStringLiteral("1.2.4"));
    QCOMPARE(restart.size(), 0);
    QVERIFY(updater.downloadedPath().isEmpty());
    QVERIFY(QDir(sandbox.filePath(QStringLiteral("cache"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    updater.check(); updater.download(); updater.install();
    QCoreApplication::processEvents();
    QCOMPARE(updater.state(), QStringLiteral("ready"));
    QCOMPARE(server.requests.size(), installedBeforeCheck ? 1 : 2);
    updater.restart();
    QCOMPARE(restart.size(), 1);
    QCOMPARE(restart.first().first().toString(), sandbox.filePath(QStringLiteral("data/workspace-native-install/current/bin/workspace-native")));
    // Readiness is not a permanent lease: validate the selected target again
    // at the explicit restart action, before the application starts shutdown.
    QVERIFY(QFile::remove(sandbox.filePath(QStringLiteral("data/workspace-native-install/versions/1.2.4/bin/workspace-native"))));
    updater.restart();
    QCOMPARE(updater.state(), QStringLiteral("error"));
    QCOMPARE(restart.size(), 1);
}

// exordy fix round (#685): the verified reuse paths (parseRelease/install reach
// "ready" through reuseInstalledVersion without a fresh install) restart into an
// already-installed version; restart must record the pairing marker then, so the
// next launch confirms the update exactly once, like a fresh install does.
void NativeUpdaterTest::restartAfterVerifiedReuseRecordsConfirmation()
{
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    Server server;
    QVERIFY(server.listening);
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        Server::reply(socket, request.startsWith("GET /latest ") ? json(server.release()) : server.archive);
    };
    QVERIFY(installedFixture(sandbox));
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), sandbox.filePath(QStringLiteral("cache")));
    QSignalSpy restart(&updater, &NativeUpdater::restartRequested);
    updater.check();
    QTRY_VERIFY(updater.state() != QStringLiteral("checking"));
    QCOMPARE(updater.state(), QStringLiteral("ready"));
    const QString storePath = QDir(sandbox.filePath(QStringLiteral("data")))
        .filePath(QStringLiteral("workspace-native-install/pending-update.conf"));
    {
        QSettings before(storePath, QSettings::IniFormat);
        QVERIFY(!before.contains(QStringLiteral("pending/version")));
    }
    updater.restart();
    QCOMPARE(restart.size(), 1);
    QSettings after(storePath, QSettings::IniFormat);
    QCOMPARE(after.value(QStringLiteral("pending/version")).toString(), QStringLiteral("1.2.4"));
    // The restarted process consumes the marker exactly once.
    NativeUpdater restarted(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.4"), sandbox.filePath(QStringLiteral("cache")));
    QCOMPARE(restarted.consumeInstalledUpdate(), QStringLiteral("1.2.4"));
    QCOMPARE(restarted.consumeInstalledUpdate(), QString());
}

void NativeUpdaterTest::invalidExistingInstallation_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"wrong-version", "missing-version", "missing-binary", "non-executable", "wrong-current", "missing-current"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void NativeUpdaterTest::invalidExistingInstallation()
{
    QFETCH(QString, mode);
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    QVERIFY(installedFixture(sandbox));
    const auto root = sandbox.filePath(QStringLiteral("data/workspace-native-install"));
    const auto target = root + QStringLiteral("/versions/1.2.4");
    if (mode == QStringLiteral("wrong-version")) QVERIFY(write(target + QStringLiteral("/VERSION"), "1.2.5\n"));
    if (mode == QStringLiteral("missing-version")) QVERIFY(QFile::remove(target + QStringLiteral("/VERSION")));
    if (mode == QStringLiteral("missing-binary")) QVERIFY(QFile::remove(target + QStringLiteral("/bin/workspace-native")));
    if (mode == QStringLiteral("non-executable")) QVERIFY(QFile::setPermissions(target + QStringLiteral("/bin/workspace-native"), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    if (mode == QStringLiteral("wrong-current") || mode == QStringLiteral("missing-current")) {
        QVERIFY(QFile::remove(root + QStringLiteral("/current")));
        if (mode == QStringLiteral("wrong-current")) {
            QVERIFY(QDir().mkpath(root + QStringLiteral("/versions/1.2.5")));
            QVERIFY(QFile::link(root + QStringLiteral("/versions/1.2.5"), root + QStringLiteral("/current")));
        }
    }
    const auto originalVersion = read(target + QStringLiteral("/VERSION"));
    const auto originalCurrent = QFileInfo(root + QStringLiteral("/current")).symLinkTarget();
    Server server;
    QVERIFY(server.listening);
    server.handler = [&](QTcpSocket *socket, const QByteArray &) { Server::reply(socket, json(server.release())); };
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), sandbox.filePath(QStringLiteral("cache")));
    QSignalSpy restart(&updater, &NativeUpdater::restartRequested);
    updater.check(); QTRY_VERIFY(updater.state() != QStringLiteral("checking"));
    QCOMPARE(updater.state(), QStringLiteral("error"));
    QVERIFY(updater.error().contains(QStringLiteral("verified")));
    updater.install(); updater.restart();
    QCOMPARE(restart.size(), 0);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(read(target + QStringLiteral("/VERSION")), originalVersion);
    QCOMPARE(QFileInfo(root + QStringLiteral("/current")).symLinkTarget(), originalCurrent);
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

void NativeUpdaterTest::downloadProgressMirrorsTheTaskbarHint_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"downloaded", "error", "cancel"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

// Reference (workspace_ui 61dacef7 main.ts:1363-1381): download-progress
// pushes percent/100 into the taskbar, and update-downloaded/error clear it
// with -1. The native mirror must reproduce that trajectory on the window's
// progress hint; see docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md.
void NativeUpdaterTest::downloadProgressMirrorsTheTaskbarHint()
{
    QFETCH(QString, mode);
    Server server;
    QVERIFY(server.listening);
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    QPointer<QTcpSocket> binary;
    const auto chunk = Archive.size() / 4; // Four equal synthetic chunks.
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (request.startsWith("GET /latest ")) { Server::reply(socket, json(server.release())); return; }
        binary = socket;
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Archive.size())
            + "\r\nConnection: close\r\n\r\n" + Archive.left(chunk));
    };
    QWindow window;
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
    UpdaterTaskbarProgress mirror;
    mirror.attach(&window, &updater);
    struct Sample { QString state; qreal hint; bool present; };
    QList<Sample> samples;
    // Connected after the mirror, so each sample observes the hint just synced.
    QObject::connect(&updater, &NativeUpdater::changed, &updater, [&] {
        const auto property = window.property("progress");
        samples.append(Sample{updater.state(), property.toDouble(), property.isValid()});
    });
    QVERIFY(!window.property("progress").isValid()); // No bar before any download event.
    updater.check();
    QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    QVERIFY(!window.property("progress").isValid()); // checking/available never push a hint.
    updater.download();
    QTRY_COMPARE(server.requests.size(), 2);
    QTRY_VERIFY(updater.progress() > 0);
    if (mode == QStringLiteral("cancel")) {
        // No Electron counterpart, but a cancelled download must not stick.
        updater.cancel();
        QCOMPARE(updater.state(), QStringLiteral("idle"));
        QCOMPARE(window.property("progress").toDouble(), qreal(-1));
        return;
    }
    const auto delivered = mode == QStringLiteral("error") ? 2 * chunk : Archive.size();
    for (auto offset = chunk; offset < delivered; offset += chunk) {
        QVERIFY(binary);
        binary->write(Archive.mid(offset, chunk));
        QTRY_VERIFY(updater.progress() >= qreal(offset + chunk) / qreal(Archive.size()));
    }
    // A complete transfer already closed the reply; the held socket may be gone.
    if (binary) binary->disconnectFromHost(); // Early close is an error, complete is downloaded.
    QTRY_VERIFY(updater.state() == QStringLiteral("downloaded") || updater.state() == QStringLiteral("error"));
    QCOMPARE(updater.state(), mode);
    QCOMPARE(window.property("progress").toDouble(), qreal(-1)); // Cleared on both edges.
    // The forwarded hint rose monotonically from 0 toward 1 inside [0, 1].
    qreal previous = 0;
    bool sawProgress = false;
    for (const auto &sample : samples) {
        if (sample.state != QStringLiteral("downloading")) continue;
        QVERIFY(sample.present);
        QVERIFY(sample.hint >= 0);
        QVERIFY(sample.hint <= 1);
        QVERIFY(sample.hint >= previous);
        previous = sample.hint;
        if (sample.hint > 0) sawProgress = true;
    }
    QVERIFY(sawProgress);
    if (mode == QStringLiteral("downloaded"))
        QCOMPARE(previous, qreal(1)); // Reached full progress before the clear.
    QVERIFY(!samples.isEmpty());
    QCOMPARE(samples.last().hint, qreal(-1)); // The final outward event cleared.
}

void NativeUpdaterTest::downloadProgressReachesTheLauncherEntryChannel_data()
{
    QTest::addColumn<QString>("mode");
    for (const auto *mode : {"downloaded", "error", "cancel", "late-launcher", "late-listener"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

// Reviewer finding 4581288189: a dynamic window property never reaches an OS
// taskbar. Progress and clear must go over a real desktop channel - the
// com.canonical.Unity.LauncherEntry.Update session-bus protocol the Unity7,
// GNOME Dash-to-Dock, Plank and Latte taskbars listen for. This case is the
// wire-level check: a fake launcher owns the well-known name and records the
// Update signals themselves, so a property-only sink records nothing and fails.
// Re-executed under dbus-run-session (the native-controller-test
// downloadedFileActions pattern) so the fake never shadows a live desktop bus.
void NativeUpdaterTest::downloadProgressReachesTheLauncherEntryChannel()
{
    QFETCH(QString, mode);
    if (!qEnvironmentVariableIsSet("CASSI_NATIVE_LAUNCHER_CHILD")) {
        if (QStandardPaths::findExecutable("dbus-run-session").isEmpty())
            QSKIP("dbus-run-session (package dbus-daemon) is not installed, so no private session bus "
                  "can host the fake LauncherEntry launcher (secure-store-platform-test precedent)");
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CASSI_NATIVE_LAUNCHER_CHILD", "1");
        environment.insert("CASSI_NATIVE_LAUNCHER_MODE", mode.toUtf8()); // The child runs only this row.
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        child.setProcessEnvironment(environment);
        child.start("dbus-run-session", { "--", QCoreApplication::applicationFilePath(),
            QStringLiteral("downloadProgressReachesTheLauncherEntryChannel") });
        QVERIFY(child.waitForFinished(90000));
        const auto output = child.readAllStandardOutput() + child.readAllStandardError();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
        return;
    }
    if (qEnvironmentVariableIsSet("CASSI_NATIVE_LAUNCHER_MODE")
        && mode != qEnvironmentVariable("CASSI_NATIVE_LAUNCHER_MODE"))
        return;
    LauncherEntryRecorder launcher;
    // A second connection to the same private bus: the bus daemon never echoes
    // a connection's own signals back to it, so the fake launcher listens on
    // its own connection while the mirror publishes over the session bus.
    const auto busAddress = qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS");
    QVERIFY2(!busAddress.isEmpty(), "dbus-run-session must provide DBUS_SESSION_BUS_ADDRESS");
    std::optional<QDBusConnection> launcherBus; // Appears when registerLauncher() below runs.
    auto releaseBus = qScopeGuard([&] {
        if (launcherBus && launcherBus->isConnected()) {
            launcherBus->unregisterObject("/com/canonical/Unity/LauncherEntry");
            launcherBus->unregisterService("com.canonical.Unity.LauncherEntry");
        }
    }); // The next data row registers its own fake on the same bus.
    const auto registerLauncher = [&] {
        if (!launcherBus || !launcherBus->isConnected())
            launcherBus = QDBusConnection::connectToBus(busAddress, "workspace-native-launcher-entry-test");
        QVERIFY2(launcherBus->isConnected(), "the fake launcher needs its own bus connection");
        QVERIFY2(launcherBus->registerService("com.canonical.Unity.LauncherEntry"),
            "the fake launcher may not already be running on this bus");
        QVERIFY(launcherBus->registerObject("/com/canonical/Unity/LauncherEntry", &launcher,
            QDBusConnection::ExportAllSlots));
        QVERIFY(launcherBus->connect(QString(), QString(),
            QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"),
            &launcher, SLOT(Update(QString, QVariantMap))));
    };
    if (mode != QStringLiteral("late-launcher") && mode != QStringLiteral("late-listener"))
        registerLauncher();

    Server server;
    QVERIFY(server.listening);
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    QPointer<QTcpSocket> binary;
    const auto chunk = Archive.size() / 4; // Four equal synthetic chunks.
    server.handler = [&](QTcpSocket *socket, const QByteArray &request) {
        if (request.startsWith("GET /latest ")) { Server::reply(socket, json(server.release())); return; }
        binary = socket;
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(Archive.size())
            + "\r\nConnection: close\r\n\r\n" + Archive.left(chunk));
    };
    QWindow window;
    NativeUpdater updater(server.url(QStringLiteral("/latest")), QStringLiteral("1.2.3"), cache.path());
    UpdaterTaskbarProgress mirror;
    mirror.attach(&window, &updater);
    QVERIFY(launcher.calls.isEmpty()); // Nothing published before any updater state.
    updater.check();
    QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    QCOMPARE(launcher.calls.size(), 0); // checking/available publish nothing, like the reference.
    updater.download();
    QTRY_COMPARE(server.requests.size(), 2);
    QTRY_VERIFY(updater.progress() > 0);
    // At least one bytes-moved frame must have crossed the bus before the
    // terminal state, so the visible-true trajectory cannot be vacuous. In
    // late-launcher and late-listener mode nothing was published yet: no
    // listener existed to record the frames the daemon dropped.
    if (mode != QStringLiteral("late-launcher") && mode != QStringLiteral("late-listener"))
        QTRY_VERIFY(std::any_of(launcher.calls.cbegin(), launcher.calls.cend(), [](const LauncherEntryRecorder::Call &call) {
            return call.properties.value(QStringLiteral("progress-visible")).toBool()
                && call.properties.value(QStringLiteral("progress")).toDouble() > 0;
        }));
    if (mode == QStringLiteral("late-launcher")) {
        // Reviewer finding 4581477136: the download stalled at this progress
        // with no launcher on the bus, so every Update frame so far was
        // dropped - signals are not replayed. When the launcher name appears,
        // the mirror must republish the current hint without waiting for more
        // bytes to move.
        QCOMPARE(launcher.calls.size(), 0);
        const qreal stalled = updater.progress();
        QVERIFY(stalled > 0.0);
        registerLauncher(); // NameOwnerChanged: no owner -> fake launcher.
        QTRY_VERIFY(!launcher.calls.isEmpty());
        const auto &call = launcher.calls.last();
        QCOMPARE(call.desktopEntry,
            QStringLiteral("application://com.exordos.workspace.native.preview.desktop"));
        QVERIFY(call.properties.value(QStringLiteral("progress-visible")).toBool());
        // The republished frame carries the current hint, not a fresh value
        // invented for the late launcher.
        QCOMPARE(call.properties.value(QStringLiteral("progress")).toDouble(), mirror.hint());
        QVERIFY(call.properties.value(QStringLiteral("progress")).toDouble() >= stalled);
        return;
    }
    if (mode == QStringLiteral("late-listener")) {
        // Reviewer finding 4581477136 round 4: real LauncherEntry consumers -
        // GNOME shell extensions per docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md -
        // never own the well-known name; they only install a sender-agnostic
        // match rule for the Update signal, so a NameOwnerChanged-based
        // republish cannot wake them. The download stalled at this progress
        // with nobody listening; a consumer that appears now must be handed
        // the current hint from a throttled periodic republish, with no new
        // bytes moving.
        QCOMPARE(launcher.calls.size(), 0);
        const qreal stalled = updater.progress();
        QVERIFY(stalled > 0.0);
        LauncherEntryRecorder listener; // Match rule only: no service name, no object.
        auto listenerBus = QDBusConnection::connectToBus(busAddress,
            QStringLiteral("workspace-native-launcher-entry-late-listener"));
        QVERIFY2(listenerBus.isConnected(), "the late listener needs its own bus connection");
        auto releaseListenerBus = qScopeGuard([&listenerBus] {
            if (listenerBus.isConnected())
                QDBusConnection::disconnectFromBus(QStringLiteral("workspace-native-launcher-entry-late-listener"));
        });
        // The D-Bus equivalent of GNOME's signal_subscribe(null, interface,
        // 'Update', null, null): empty service and path mean "every sender,
        // every path" - exactly how a consumer that never owns the name listens.
        QVERIFY(listenerBus.connect(QString(), QString(),
            QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"),
            &listener, SLOT(Update(QString, QVariantMap))));
        // Bounded delivery: nothing writes to the socket from here on, so only
        // a periodic republish of the current hint can arrive, within the
        // QTRY window at worst.
        QTRY_VERIFY(!listener.calls.isEmpty());
        const auto &late = listener.calls.last();
        QCOMPARE(late.desktopEntry,
            QStringLiteral("application://com.exordos.workspace.native.preview.desktop"));
        QVERIFY(late.properties.value(QStringLiteral("progress-visible")).toBool());
        QCOMPARE(late.properties.value(QStringLiteral("progress")).toDouble(), mirror.hint());
        QVERIFY(late.properties.value(QStringLiteral("progress")).toDouble() >= stalled);
        // Quiet, not a broadcast storm: over a fixed 6 s window at most a few
        // frames may arrive (this brackets the republish interval to roughly
        // 1.5 s or slower on average; the QTRY above already bounded it to
        // at most 5 s).
        const auto framesAtWindowStart = listener.calls.size();
        QTest::qWait(6000);
        QVERIFY2(listener.calls.size() - framesAtWindowStart <= 4,
            qPrintable(QStringLiteral("late listener took %1 extra frames in 6 s: not throttled")
                .arg(listener.calls.size() - framesAtWindowStart)));
        // Bounded lifetime: the terminal state must end the republish. The
        // listener records the clear frame, then silence.
        updater.cancel();
        QTRY_VERIFY(!listener.calls.last().properties.value(QStringLiteral("progress-visible")).toBool());
        const auto framesUntilClear = listener.calls.size();
        QTest::qWait(4000);
        QCOMPARE(listener.calls.size(), framesUntilClear);
        return;
    }
    if (mode == QStringLiteral("cancel")) {
        updater.cancel(); // No Electron counterpart, but a cancelled download must not stick.
    } else {
        const auto delivered = mode == QStringLiteral("error") ? 2 * chunk : Archive.size();
        for (auto offset = chunk; offset < delivered; offset += chunk) {
            QVERIFY(binary);
            binary->write(Archive.mid(offset, chunk));
            QTRY_VERIFY(updater.progress() >= qreal(offset + chunk) / qreal(Archive.size()));
        }
        if (binary) binary->disconnectFromHost(); // Early close is an error, complete is downloaded.
        QTRY_VERIFY(updater.state() == QStringLiteral("downloaded") || updater.state() == QStringLiteral("error"));
    }
    QTRY_VERIFY(!launcher.calls.isEmpty()
        && !launcher.calls.last().properties.value(QStringLiteral("progress-visible")).toBool());
    if (mode != QStringLiteral("cancel")) QCOMPARE(updater.state(), mode);
    // Every wire message names the installed desktop entry through the
    // protocol's application:// app-uri...
    for (const auto &call : launcher.calls)
        QCOMPARE(call.desktopEntry,
            QStringLiteral("application://com.exordos.workspace.native.preview.desktop"));
    // ...the visible progress values stay inside [0, 1] and never fall behind...
    qreal previous = 0;
    bool sawProgress = false;
    for (const auto &call : launcher.calls) {
        const auto properties = call.properties;
        if (!properties.value(QStringLiteral("progress-visible")).toBool()) continue;
        QVERIFY(properties.contains(QStringLiteral("progress")));
        const auto progress = properties.value(QStringLiteral("progress")).toDouble();
        QVERIFY(progress >= 0.0);
        QVERIFY(progress <= 1.0);
        QVERIFY(progress >= previous);
        previous = progress;
        if (progress > 0) sawProgress = true;
    }
    QVERIFY(sawProgress); // The fake launcher actually saw bytes move.
    if (mode == QStringLiteral("downloaded"))
        QCOMPARE(previous, qreal(1)); // Full progress reached the launcher before the clear.
    // The last published frame is the clear: progress-visible=false and no
    // stale progress value (the final state must not leave a stuck bar).
    const auto &last = launcher.calls.last();
    QCOMPARE(last.properties.value(QStringLiteral("progress-visible")).toBool(), false);
    QVERIFY(!last.properties.contains(QStringLiteral("progress")));
}

// The window progress hint case needs a real QWindow, so the suite runs on
// QGuiApplication; forcing offscreen keeps every case headless as before.
void NativeUpdaterTest::postInstallConfirmationPairing_data()
{
    QTest::addColumn<QString>("current");
    QTest::addColumn<QString>("remembered"); // null: remember nothing
    QTest::addColumn<QString>("rawOverride"); // non-null: write verbatim (corrupt case)
    QTest::addColumn<QString>("expected");
    QTest::newRow("exact match") << "0.44.0" << "0.44.0" << QString() << "0.44.0";
    QTest::newRow("v prefix normalized") << "0.44.0" << "v0.44.0" << QString() << "0.44.0";
    QTest::newRow("mismatch clears silently") << "0.44.0" << "0.43.0" << QString() << QString();
    QTest::newRow("empty remember is a no-op") << "0.44.0" << QString() << QString() << QString();
    QTest::newRow("corrupt value clears silently") << "0.44.0" << QString()
        << QStringLiteral("not-a-version{") << QString();
    QTest::newRow("no marker stays silent") << "0.44.0" << QString() << QString() << QString();
}

void NativeUpdaterTest::postInstallConfirmationPairing()
{
    QFETCH(QString, current);
    QFETCH(QString, remembered);
    QFETCH(QString, rawOverride);
    QFETCH(QString, expected);
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());
    const Environment data("XDG_DATA_HOME", sandbox.filePath(QStringLiteral("data")).toUtf8());
    NativeUpdater updater(QUrl(QStringLiteral("http://127.0.0.1:1/latest")), current, cache.path());
    // The marker anchors at the managed install root (#634): it must survive
    // the post-install cache purge, which the install cases pin empty.
    const QString storePath = QDir(sandbox.filePath(QStringLiteral("data")))
        .filePath(QStringLiteral("workspace-native-install/pending-update.conf"));
    if (!rawOverride.isNull()) {
        QSettings store(storePath, QSettings::IniFormat);
        store.setValue(QStringLiteral("pending/version"), rawOverride);
        store.sync();
    } else if (!remembered.isNull()) {
        updater.rememberPendingUpdate(remembered);
    }
    QCOMPARE(updater.consumeInstalledUpdate(), expected);
    // One-shot: the marker is consumed even on mismatch, so a confirmation never repeats.
    QCOMPARE(updater.consumeInstalledUpdate(), QString());
    QSettings after(storePath, QSettings::IniFormat);
    QVERIFY(!after.contains(QStringLiteral("pending/version")));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    NativeUpdaterTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "native-updater-test.moc"
