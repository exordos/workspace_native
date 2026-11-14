#include "native-logging.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStandardPaths>

#include <cstdio>

namespace workspace {
namespace {

constexpr char LogDirName[] = "workspace_logs";
constexpr char LogFileName[] = "workspace.log";
constexpr char RotatedLogFileName[] = "workspace.log.1";

const char *messageLevelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info";
    case QtWarningMsg: return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg: return "fatal";
    }
    return "debug";
}

NativeLogging *g_messageLogSink = nullptr;

// qInstallMessageHandler replaces Qt's default handler, so the console output
// is reproduced explicitly (stderr, like the Qt default) instead of chained.
void nativeMessageHandler(QtMsgType type, const QMessageLogContext &context,
                          const QString &message)
{
    Q_UNUSED(context);
    const QString line = NativeLogging::formatMessageLine(type, message);
    const QByteArray utf8 = line.toUtf8();
    if (fwrite(utf8.constData(), 1, size_t(utf8.size()), stderr) != size_t(utf8.size()))
        return;
    fputc('\n', stderr);
    const int flushResult [[maybe_unused]] = fflush(stderr);
    if (g_messageLogSink)
        g_messageLogSink->appendLogLine(line);
}

qint64 parseProcKbValue(const QByteArray &contents, const QByteArray &key)
{
    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray &line : lines) {
        const int colon = line.indexOf(':');
        if (colon < 0 || line.left(colon).trimmed() != key)
            continue;
        bool ok = false;
        const qint64 value = line.mid(colon + 1).trimmed().split(' ').first().toLongLong(&ok);
        return ok && value > 0 ? value : 0;
    }
    return 0;
}

QByteArray readProcFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// JavaScript Date.toISOString() equivalent: UTC with milliseconds, Z suffix.
QString isoTimestampNow()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"))
        + QLatin1Char('Z');
}

} // namespace

NativeLogging::NativeLogging(QString rootDir, QObject *parent)
    : QObject(parent)
{
    if (rootDir.isEmpty())
        rootDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_rootDir = std::move(rootDir);
}

QString NativeLogging::logDirectoryPath() const
{
    return m_rootDir + QLatin1Char('/') + QLatin1String(LogDirName);
}

QString NativeLogging::filePath() const
{
    return logDirectoryPath() + QLatin1Char('/') + QLatin1String(LogFileName);
}

QString NativeLogging::rotatedFilePath() const
{
    return logDirectoryPath() + QLatin1Char('/') + QLatin1String(RotatedLogFileName);
}

void NativeLogging::setProcRoot(QString procRoot)
{
    m_procRoot = std::move(procRoot);
}

bool NativeLogging::ensureLogDirectory()
{
    return QDir().mkpath(logDirectoryPath());
}

QString NativeLogging::getFilePath()
{
    QMutexLocker locker(&m_ioMutex);
    if (!ensureLogDirectory())
        return {};
    return filePath();
}

bool NativeLogging::appendLogLine(const QString &rawLine)
{
    // Reference main.ts:162-182: strip control chars (tab, LF and CR survive),
    // slice to the 32768-char budget, then trim; empty lines write nothing.
    QString sanitizedLine = rawLine;
    sanitizedLine.removeIf([](QChar character) {
        const ushort code = character.unicode();
        return code <= 0x08 || code == 0x0B || code == 0x0C
            || (code >= 0x0E && code <= 0x1F) || code == 0x7F;
    });
    if (sanitizedLine.size() > MaxLogLineLength)
        sanitizedLine.truncate(int(MaxLogLineLength));
    sanitizedLine = sanitizedLine.trimmed();
    if (sanitizedLine.isEmpty())
        return false;
    const QByteArray payload =
        (isoTimestampNow() + QLatin1Char(' ') + sanitizedLine + QLatin1Char('\n')).toUtf8();
    QMutexLocker locker(&m_ioMutex);
    if (!ensureLogDirectory())
        return false;
    rotateLogsIfNeeded();
    QFile file(filePath());
    if (!file.open(QIODevice::Append))
        return false;
    const bool wrote = file.write(payload) == payload.size();
    file.close();
    return wrote;
}

void NativeLogging::rotateLogsIfNeeded()
{
    const QFileInfo info(filePath());
    if (!info.exists() || info.size() < MaxLogFileBytes)
        return;
    QFile::remove(rotatedFilePath());
    QFile::rename(filePath(), rotatedFilePath());
}

QVariantMap NativeLogging::memorySnapshot() const
{
    // Portable procfs sources (Linux is the acceptance target; other platforms
    // keep the same key set with zero values — see the design doc note).
    const QByteArray status = readProcFile(m_procRoot + QStringLiteral("/self/status"));
    const QByteArray meminfo = readProcFile(m_procRoot + QStringLiteral("/meminfo"));
    const qint64 rssKb = parseProcKbValue(status, "VmRSS");
    const qint64 sizeKb = parseProcKbValue(status, "VmSize");
    const qint64 peakRssKb = parseProcKbValue(status, "VmHWM");
    QVariantMap mainMemory;
    mainMemory.insert("rss", rssKb * 1024);   // bytes, like process.memoryUsage()
    mainMemory.insert("size", sizeKb * 1024);
    QVariantMap system;
    system.insert("total", parseProcKbValue(meminfo, "MemTotal"));
    system.insert("free", parseProcKbValue(meminfo, "MemFree"));
    system.insert("available", parseProcKbValue(meminfo, "MemAvailable"));
    QVariantMap selfMemory;
    selfMemory.insert("workingSetSize", rssKb);          // kB, like getAppMetrics()
    selfMemory.insert("peakWorkingSetSize", peakRssKb);
    QVariantMap selfProcess;
    selfProcess.insert("pid", QCoreApplication::applicationPid());
    selfProcess.insert("type", QStringLiteral("Main"));
    selfProcess.insert("memory", selfMemory);
    QVariantMap snapshot;
    snapshot.insert("collectedAt", isoTimestampNow());
    snapshot.insert("main", mainMemory);
    snapshot.insert("system", system);
    snapshot.insert("processes", QVariantList{selfProcess});
    snapshot.insert("totalWorkingSetKb", rssKb);
    return snapshot;
}

QString NativeLogging::formatMessageLine(QtMsgType type, const QString &message)
{
    return QString::fromLatin1(messageLevelName(type)) + QStringLiteral(": ") + message;
}

void installNativeLogHandler(NativeLogging &logging)
{
    g_messageLogSink = &logging;
    qInstallMessageHandler(nativeMessageHandler);
}

void removeNativeLogHandler()
{
    qInstallMessageHandler(nullptr);
    g_messageLogSink = nullptr;
}

} // namespace workspace
