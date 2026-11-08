#include "native-updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QVersionNumber>

#include <cmath>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace workspace {
namespace {
const QUrl ReleaseUrl(QStringLiteral("https://api.github.com/repos/exordos/workspace_native/releases/latest"));
constexpr qint64 ReadChunk = 64 * 1024;
constexpr qint64 MaximumListingBytes = 16 * 1024 * 1024;
constexpr quint64 MaximumExpandedBytes = 4ULL * 1024 * 1024 * 1024;

QString installRoot()
{
    const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return data.isEmpty() ? QString{} : QDir(data).filePath(QStringLiteral("workspace-native-install"));
}

QString cacheRoot()
{
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return cache.isEmpty() ? QString{} : QDir(cache).filePath(QStringLiteral("updates"));
}

QVersionNumber version(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
    if (!pattern.match(text).hasMatch()) return {};
    qsizetype suffix = 0;
    const auto result = QVersionNumber::fromString(text, &suffix);
    return suffix == text.size() && result.segmentCount() == 3 ? result : QVersionNumber{};
}
bool cleanUrl(const QUrl &url)
{
    return url.isValid() && !url.isRelative() && url.userInfo().isEmpty()
        && !url.hasFragment() && !url.host().isEmpty();
}
bool loopback(const QUrl &url)
{
    return cleanUrl(url) && url.scheme() == QStringLiteral("http") && url.port() > 0
        && (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("::1"));
}
}

NativeUpdater::NativeUpdater(QObject *parent)
    : NativeUpdater(ReleaseUrl, QCoreApplication::applicationVersion(),
                    cacheRoot(), parent)
{
}

NativeUpdater::NativeUpdater(const QUrl &releaseUrl, const QString &currentVersion,
                             const QString &cachePath, QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)),
      m_releaseUrl(releaseUrl), m_currentVersion(currentVersion), m_cachePath(cachePath),
      m_fixture(loopback(releaseUrl)), m_endpointValid(releaseUrl == ReleaseUrl || m_fixture)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail(m_ticket, tr("The update operation timed out."));
    });
}

NativeUpdater::~NativeUpdater()
{
    ++m_ticket;
    m_deadline.stop();
    stopReply();
    stopProcess();
    discardFile();
}

bool NativeUpdater::busy() const
{
    return m_state == QStringLiteral("checking") || m_state == QStringLiteral("downloading")
        || m_state == QStringLiteral("installing");
}

void NativeUpdater::stopReply()
{
    auto reply = m_reply;
    m_reply = nullptr; // Invalidate before abort can synchronously signal.
    if (!reply) return;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}

void NativeUpdater::discardFile()
{
    m_downloadedPath.clear();
    m_file.reset(); // Own temporary files only, including a verified previous download.
}

void NativeUpdater::cancel()
{
    ++m_ticket;
    m_deadline.stop();
    stopReply();
    stopProcess();
    m_staging.reset();
    discardFile();
    m_metadata.clear();
    m_availableVersion.clear();
    m_downloadUrl.clear();
    m_expectedSha256.clear();
    m_expectedSize = 0;
    m_progress = 0;
    m_error.clear();
    m_state = QStringLiteral("idle");
    emit changed();
}

void NativeUpdater::fail(quint64 ticket, const QString &message)
{
    if (ticket != m_ticket) return;
    m_deadline.stop();
    stopReply();
    stopProcess();
    m_staging.reset();
    discardFile();
    m_metadata.clear();
    m_progress = 0;
    m_error = message; // Never expose a response body, signed URL or process output.
    m_state = QStringLiteral("error");
    emit changed();
}

void NativeUpdater::check()
{
    if (busy() || m_state == QStringLiteral("downloaded") || m_state == QStringLiteral("ready")) return;
    const auto ticket = ++m_ticket;
    discardFile();
    m_availableVersion.clear();
    m_downloadUrl.clear();
    m_expectedSha256.clear();
    m_expectedSize = 0;
    m_metadata.clear();
    m_error.clear();
    m_progress = 0;
    if (!m_endpointValid || version(m_currentVersion).isNull()) {
        fail(ticket, tr("The update configuration is invalid."));
        return;
    }
#if !defined(Q_OS_LINUX)
    fail(ticket, tr("Updates are not available for this platform."));
    return;
#else
    if (QSysInfo::currentCpuArchitecture() != QStringLiteral("x86_64")) {
        fail(ticket, tr("Updates are not available for this platform."));
        return;
    }
#endif
    m_state = QStringLiteral("checking");
    QPointer<NativeUpdater> guard(this);
    emit changed();
    if (!guard || ticket != m_ticket || m_state != QStringLiteral("checking")) return;
    m_deadline.start(30'000);
    startRequest(ticket, m_releaseUrl, true);
}

bool NativeUpdater::allowedDownload(const QUrl &url, bool redirect) const
{
    if (!cleanUrl(url)) return false;
    if (m_fixture) return loopback(url) && url.host() == m_releaseUrl.host()
        && url.port() == m_releaseUrl.port();
    if (url.scheme() != QStringLiteral("https") || url.port(443) != 443) return false;
    if (redirect) return url.host() == QStringLiteral("release-assets.githubusercontent.com")
        || url.host() == QStringLiteral("objects.githubusercontent.com");
    return url.host() == QStringLiteral("github.com") && !url.hasQuery();
}

void NativeUpdater::download()
{
    if (busy() || m_downloadUrl.isEmpty() || m_expectedSize <= 0
        || m_expectedSha256.size() != 64) return;
    const auto ticket = ++m_ticket;
    discardFile();
    m_error.clear();
    m_progress = 0;
    m_received = 0;
    m_redirects = 0;
    m_hash.reset();
    if (m_cachePath.isEmpty() || !QDir().mkpath(m_cachePath)) {
        fail(ticket, tr("The update download could not be saved."));
        return;
    }
    m_file = std::make_unique<QTemporaryFile>(QDir(m_cachePath).filePath(
        m_assetName + QStringLiteral(".XXXXXX.part")));
    if (!m_file->open()) {
        fail(ticket, tr("The update download could not be saved."));
        return;
    }
    m_state = QStringLiteral("downloading");
    QPointer<NativeUpdater> guard(this);
    emit changed();
    if (!guard || ticket != m_ticket || m_state != QStringLiteral("downloading")) return;
    m_deadline.start(300'000);
    startRequest(ticket, m_downloadUrl, false);
}

void NativeUpdater::startRequest(quint64 ticket, const QUrl &url, bool metadata)
{
    if (ticket != m_ticket) return;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("User-Agent", "Exordos-Workspace-Native/" + m_currentVersion.toLatin1());
    request.setRawHeader("Accept", metadata ? "application/vnd.github+json" : "application/octet-stream");
    request.setRawHeader("Accept-Encoding", "identity");
    if (metadata) request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setTransferTimeout(30'000);
    auto *reply = m_network->get(request);
    m_reply = reply;
    reply->setReadBufferSize(ReadChunk);
    connect(reply, &QNetworkReply::readyRead, this, [this, ticket, reply, metadata] {
        consume(ticket, reply, metadata);
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, ticket, reply, metadata] {
        if (ticket != m_ticket || m_reply != reply) return;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status && status != 200) {
            // Reject/follow at headers; a large error/redirect body must not fill
            // the bounded read buffer and prevent finished() from being reached.
            finished(ticket, reply, metadata);
            return;
        }
        const auto encoding = reply->rawHeader("Content-Encoding").toLower();
        if (!encoding.isEmpty() && encoding != "identity") {
            fail(ticket, tr("The update response encoding was rejected."));
            return;
        }
        const auto declared = reply->header(QNetworkRequest::ContentLengthHeader);
        const auto limit = metadata ? MaximumMetadataBytes : m_expectedSize;
        if (declared.isValid() && (declared.toLongLong() < 0 || declared.toLongLong() > limit))
            fail(ticket, tr("The update response is too large."));
    });
    connect(reply, &QNetworkReply::finished, this, [this, ticket, reply, metadata] {
        finished(ticket, reply, metadata);
    });
}

void NativeUpdater::consume(quint64 ticket, QNetworkReply *reply, bool metadata)
{
    if (ticket != m_ticket || m_reply != reply) return;
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
    while (reply->bytesAvailable() > 0) {
        const auto chunk = reply->read(ReadChunk);
        if (chunk.isEmpty()) break;
        if (metadata) {
            if (chunk.size() > MaximumMetadataBytes - m_metadata.size()) {
                fail(ticket, tr("The update response is too large."));
                return;
            }
            m_metadata += chunk;
        } else {
            if (!m_file || chunk.size() > m_expectedSize - m_received
                || m_received + chunk.size() > MaximumArchiveBytes) {
                fail(ticket, tr("The update response is too large."));
                return;
            }
            if (m_file->write(chunk) != chunk.size()) {
                fail(ticket, tr("The update download could not be saved."));
                return;
            }
            m_hash.addData(chunk);
            m_received += chunk.size();
        }
    }
    if (!metadata && m_received > 0) {
        m_progress = qreal(m_received) / qreal(m_expectedSize);
        emit changed(); // Caller checks the operation again after this outward notification.
    }
}

void NativeUpdater::finished(quint64 ticket, QNetworkReply *reply, bool metadata)
{
    if (ticket != m_ticket || m_reply != reply) return;
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!metadata && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
        const auto next = reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        if (++m_redirects > 5 || !allowedDownload(next, true)) {
            fail(ticket, tr("The update download redirect was rejected."));
            return;
        }
        stopReply();
        startRequest(ticket, next, false);
        return;
    }
    if (status != 200 || reply->error() != QNetworkReply::NoError) {
        fail(ticket, tr("The update request failed."));
        return;
    }
    QPointer<NativeUpdater> guard(this);
    consume(ticket, reply, metadata);
    if (!guard || ticket != m_ticket || m_reply != reply) return;
    m_reply = nullptr;
    reply->deleteLater();
    m_deadline.stop();
    if (metadata) {
        parseRelease(ticket);
        return;
    }
    if (m_received != m_expectedSize || m_hash.result().toHex() != m_expectedSha256) {
        fail(ticket, tr("The downloaded update failed verification."));
        return;
    }
    if (!m_file || !m_file->flush()) {
        fail(ticket, tr("The update download could not be saved."));
        return;
    }
    m_file->close();
    if (!m_file->setPermissions(QFileDevice::ReadOwner)) {
        fail(ticket, tr("The update download could not be saved."));
        return;
    }
    m_downloadedPath = m_file->fileName();
    m_progress = 1;
    m_state = QStringLiteral("downloaded");
    emit changed();
}

void NativeUpdater::stopProcess()
{
    auto process = m_process;
    m_process = nullptr;
    if (!process) return;
    disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning) {
        // Let the installer run its TERM/EXIT cleanup before using a hard stop.
        // Each updater subprocess owns its Unix session, including its children.
        bool sentToGroup = false;
        const auto pid = process->processId();
#ifdef Q_OS_UNIX
        if (pid > 0) sentToGroup = ::kill(-pid, SIGTERM) == 0;
#endif
        if (!sentToGroup) process->terminate();
        if (!process->waitForFinished(1000) && process->state() != QProcess::NotRunning) {
#ifdef Q_OS_UNIX
            if (pid > 0) ::kill(-pid, SIGKILL);
#endif
            process->kill();
            process->waitForFinished(1000);
        }
    }
    process->deleteLater();
}

void NativeUpdater::install()
{
    if (m_state != QStringLiteral("downloaded") || !m_file || m_downloadedPath.isEmpty()) return;
    const auto ticket = ++m_ticket;
    m_error.clear();
    m_progress = 0;
    m_staging = std::make_unique<QTemporaryDir>(QDir(m_cachePath).filePath(QStringLiteral("install-XXXXXX")));
    if (!m_staging->isValid() || installRoot().isEmpty()) {
        fail(ticket, tr("The update could not be prepared."));
        return;
    }
    m_state = QStringLiteral("installing");
    QPointer<NativeUpdater> guard(this);
    emit changed();
    if (!guard || ticket != m_ticket || m_state != QStringLiteral("installing")) return;
    startProcess(ticket, QStringLiteral("list"), QStringLiteral("/usr/bin/tar"),
        {QStringLiteral("--list"), QStringLiteral("--verbose"), QStringLiteral("--numeric-owner"),
         QStringLiteral("--full-time"), QStringLiteral("--quoting-style=escape"),
         QStringLiteral("--gzip"), QStringLiteral("--file"), m_downloadedPath});
}

void NativeUpdater::startProcess(quint64 ticket, const QString &phase,
                                 const QString &program, const QStringList &arguments)
{
    if (ticket != m_ticket || !m_staging) return;
    m_installPhase = phase;
    m_listing.clear();
    m_processOutputBytes = 0;
    auto *process = new QProcess(this);
    m_process = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &name : {"TAR_OPTIONS", "GZIP", "BASH_ENV", "ENV"}) environment.remove(QString::fromLatin1(name));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(m_staging->path());
#ifdef Q_OS_UNIX
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession
        | QProcess::UnixProcessFlag::DisconnectControllingTerminal);
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, [this, ticket, process] { if (m_process == process) readProcess(ticket); });
    connect(process, &QProcess::readyReadStandardError, this, [this, ticket, process] { if (m_process == process) readProcess(ticket); });
    connect(process, &QProcess::errorOccurred, this, [this, ticket, process](QProcess::ProcessError error) {
        if (ticket == m_ticket && m_process == process && error == QProcess::FailedToStart)
            fail(ticket, tr("The update installer could not be started."));
    });
    connect(process, &QProcess::finished, this, [this, ticket, process](int exitCode, QProcess::ExitStatus status) {
        if (m_process == process) processFinished(ticket, exitCode, status == QProcess::NormalExit);
    });
    m_deadline.start(phase == QStringLiteral("list") ? 60'000 : 300'000);
    process->start(program, arguments, QIODevice::ReadOnly);
}

void NativeUpdater::readProcess(quint64 ticket)
{
    if (ticket != m_ticket || !m_process) return;
    const auto output = m_process->readAllStandardOutput();
    const auto errors = m_process->readAllStandardError();
    m_processOutputBytes += output.size() + errors.size();
    if (m_processOutputBytes > MaximumListingBytes) {
        fail(ticket, tr("The update installer produced too much output."));
        return;
    }
    if (m_installPhase == QStringLiteral("list")) m_listing += output;
}

bool NativeUpdater::validateListing() const
{
    const auto root = QStringLiteral("workspace-native-%1-linux-x86_64").arg(m_availableVersion);
    // GNU tar --full-time --numeric-owner output, with the C locale. Reject
    // escaped names instead of attempting to reinterpret control characters.
    static const QRegularExpression linePattern(QStringLiteral(
        "^([dl-])[rwxStTs-]{9} [0-9]+/[0-9]+ +([0-9]+) "
        "[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]+)? (.+)$"));
    static const QRegularExpression pathPattern(QStringLiteral("^[A-Za-z0-9_./+@-]+$"));
    QSet<QString> members;
    QSet<QString> files;
    QSet<QString> links;
    quint64 expanded = 0;
    for (const auto &raw : m_listing.split('\n')) {
        if (raw.isEmpty()) continue;
        const auto match = linePattern.match(QString::fromLatin1(raw));
        if (!match.hasMatch() || members.size() >= 100'000) return false;
        const auto type = match.captured(1);
        auto path = match.captured(3);
        if (type == QStringLiteral("l")) {
            const auto parts = path.split(QStringLiteral(" -> "));
            if (parts.size() != 2) return false;
            path = parts.first();
            const auto target = parts.last();
            if (target.size() > 4096 || !pathPattern.match(target).hasMatch() || target.startsWith('/')
                || target.split('/').contains(QStringLiteral(".."))) return false;
            const auto resolved = QDir::cleanPath(QFileInfo(path).path() + '/' + target);
            if (!resolved.startsWith(root + '/')) return false;
        }
        while (path.endsWith('/')) path.chop(1);
        if (path.size() > 4096 || path.count('/') > 64 || !pathPattern.match(path).hasMatch() || path.startsWith('/')
            || path.split('/').contains(QStringLiteral("..")) || QDir::cleanPath(path) != path
            || (path != root && !path.startsWith(root + '/')) || members.contains(path)) return false;
        bool sizeOkay = false;
        const auto size = match.captured(2).toULongLong(&sizeOkay);
        if (!sizeOkay || size > MaximumExpandedBytes - expanded) return false;
        expanded += size;
        members.insert(path);
        if (type == QStringLiteral("-")) files.insert(path);
        if (type == QStringLiteral("l")) links.insert(path);
    }
    for (const auto &path : members) {
        auto parent = QFileInfo(path).path();
        while (parent != QStringLiteral(".")) {
            if (links.contains(parent)) return false;
            parent = QFileInfo(parent).path();
        }
    }
    return files.contains(root + QStringLiteral("/VERSION"))
        && files.contains(root + QStringLiteral("/install.sh"))
        && files.contains(root + QStringLiteral("/bin/workspace-native"));
}

void NativeUpdater::processFinished(quint64 ticket, int exitCode, bool normalExit)
{
    if (ticket != m_ticket || !m_process) return;
    QPointer<NativeUpdater> guard(this);
    readProcess(ticket);
    if (!guard || ticket != m_ticket || !m_process || m_state != QStringLiteral("installing")) return;
    m_deadline.stop();
    auto process = m_process;
    m_process = nullptr;
    process->deleteLater();
    if (!normalExit || exitCode != 0) {
        fail(ticket, tr("The update installation failed."));
        return;
    }
    const auto root = QStringLiteral("workspace-native-%1-linux-x86_64").arg(m_availableVersion);
    if (m_installPhase == QStringLiteral("list")) {
        if (!validateListing()) {
            fail(ticket, tr("The update archive layout is invalid."));
            return;
        }
        startProcess(ticket, QStringLiteral("extract"), QStringLiteral("/usr/bin/tar"),
            {QStringLiteral("--extract"), QStringLiteral("--gzip"), QStringLiteral("--file"), m_downloadedPath,
             QStringLiteral("--directory"), m_staging->path(), QStringLiteral("--no-same-owner"),
             QStringLiteral("--no-same-permissions"), QStringLiteral("--keep-old-files")});
        return;
    }
    if (m_installPhase == QStringLiteral("extract")) {
        const auto prefix = QDir(m_staging->path()).filePath(root);
        QFile versionFile(QDir(prefix).filePath(QStringLiteral("VERSION")));
        const QFileInfo script(QDir(prefix).filePath(QStringLiteral("install.sh")));
        const QFileInfo binary(QDir(prefix).filePath(QStringLiteral("bin/workspace-native")));
        if (!versionFile.open(QIODevice::ReadOnly) || versionFile.size() > 64
            || QString::fromLatin1(versionFile.readAll()).trimmed() != m_availableVersion
            || !script.isFile() || script.isSymLink() || !binary.isFile() || binary.isSymLink()) {
            fail(ticket, tr("The update archive layout is invalid."));
            return;
        }
        startProcess(ticket, QStringLiteral("apply"), QStringLiteral("/bin/bash"),
            {script.absoluteFilePath(), QStringLiteral("--no-launch")});
        return;
    }
    if (!installedVersionMatches()) {
        fail(ticket, tr("The installed update could not be verified."));
        return;
    }
    m_staging.reset();
    discardFile();
    m_progress = 1;
    m_state = QStringLiteral("ready");
    emit changed();
}

bool NativeUpdater::installedVersionMatches() const
{
    const auto root = installRoot();
    if (root.isEmpty()) return false;
    const auto expected = QFileInfo(QDir(root).filePath(QStringLiteral("versions/") + m_availableVersion)).canonicalFilePath();
    const auto current = QFileInfo(QDir(root).filePath(QStringLiteral("current"))).canonicalFilePath();
    if (expected.isEmpty() || current != expected) return false;
    QFile text(QDir(current).filePath(QStringLiteral("VERSION")));
    const QFileInfo binary(QDir(current).filePath(QStringLiteral("bin/workspace-native")));
    return text.open(QIODevice::ReadOnly) && text.size() <= 64
        && QString::fromLatin1(text.readAll()).trimmed() == m_availableVersion
        && binary.isFile() && binary.isExecutable();
}

void NativeUpdater::restart()
{
    if (m_state != QStringLiteral("ready")) return;
    if (!installedVersionMatches()) {
        fail(m_ticket, tr("The installed update could not be verified."));
        return;
    }
    emit restartRequested(QDir(installRoot()).filePath(QStringLiteral("current/bin/workspace-native")));
}

void NativeUpdater::parseRelease(quint64 ticket)
{
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(m_metadata, &parseError);
    m_metadata.clear();
    const auto release = document.object();
    const auto tag = release.value(QStringLiteral("tag_name")).toString();
    const auto normalized = tag.startsWith('v') ? tag.mid(1) : tag;
    const auto candidate = version(normalized);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !release.value(QStringLiteral("draft")).isBool() || release.value(QStringLiteral("draft")).toBool()
        || !release.value(QStringLiteral("prerelease")).isBool() || release.value(QStringLiteral("prerelease")).toBool()
        || candidate.isNull()) {
        fail(ticket, tr("The update release metadata is invalid."));
        return;
    }
    if (QVersionNumber::compare(candidate, version(m_currentVersion)) <= 0) {
        m_state = QStringLiteral("current");
        emit changed();
        return;
    }
    const auto name = QStringLiteral("Exordos-Workspace-Native-%1-linux-x86_64.tar.gz").arg(normalized);
    QJsonObject asset;
    int matches = 0;
    for (const auto &value : release.value(QStringLiteral("assets")).toArray()) {
        const auto object = value.toObject();
        if (object.value(QStringLiteral("name")).toString() == name) { asset = object; ++matches; }
    }
    const auto sizeValue = asset.value(QStringLiteral("size"));
    const auto size = sizeValue.toDouble(-1);
    const auto digest = asset.value(QStringLiteral("digest")).toString();
    const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
    static const QRegularExpression hashPattern(QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
    const auto hashMatch = hashPattern.match(digest);
    const auto expectedUrl = QStringLiteral("https://github.com/exordos/workspace_native/releases/download/%1/%2").arg(tag, name);
    if (matches != 1 || !sizeValue.isDouble() || !std::isfinite(size) || std::floor(size) != size
        || size <= 0 || size > MaximumArchiveBytes || !hashMatch.hasMatch()
        || !allowedDownload(url, false) || (!m_fixture && url.toString(QUrl::FullyEncoded) != expectedUrl)) {
        fail(ticket, tr("The update release metadata is invalid."));
        return;
    }
    m_availableVersion = normalized;
    m_assetName = name;
    m_downloadUrl = url;
    m_expectedSha256 = hashMatch.captured(1).toLatin1().toLower();
    m_expectedSize = qint64(size);
    m_state = QStringLiteral("available");
    emit changed();
}

} // namespace workspace
