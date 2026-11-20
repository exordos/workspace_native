#include "native-diagnostics.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>

#include <optional>
#include <cmath>
#include <limits>
#include <algorithm>

#if defined(Q_OS_LINUX)
#  include <unistd.h> // sysconf(_SC_CLK_TCK) for the memory provider CPU math
#endif

// The diagnostics collector core up top is header-only pure math (see the
// header); this translation unit holds the issue #51 session-log code: the
// logger.ts redact() port, the reference filter/export semantics and the
// in-memory ring buffer.
namespace workspace::diagnostics {

namespace {

// logger.ts SENSITIVE_KEYS (matched after lowercase + '-/'_'-strip).
const QSet<QString> &sensitiveKeys()
{
    static const QSet<QString> keys{
        QStringLiteral("apikey"), QStringLiteral("password"), QStringLiteral("passwd"),
        QStringLiteral("secret"), QStringLiteral("token"), QStringLiteral("authorization"),
        QStringLiteral("auth"), QStringLiteral("cookie"), QStringLiteral("session"),
        QStringLiteral("sessionid"), QStringLiteral("csrf"), QStringLiteral("csrftoken"),
        QStringLiteral("creditcard"), QStringLiteral("ssn"), QStringLiteral("privatekey")};
    return keys;
}

// logger.ts SENSITIVE_PATTERNS: long base64-ish runs and Basic/Bearer values.
bool isSensitiveValue(const QString &value)
{
    static const QRegularExpression base64Run(
        QStringLiteral("^[A-Za-z0-9+/=]{20,}$"));
    static const QRegularExpression basicPrefix(
        QStringLiteral("^Basic\\s+"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bearerPrefix(
        QStringLiteral("^Bearer\\s+"), QRegularExpression::CaseInsensitiveOption);
    return base64Run.match(value).hasMatch() || basicPrefix.match(value).hasMatch()
        || bearerPrefix.match(value).hasMatch();
}

bool isSensitiveKey(const QString &key)
{
    auto normalized = key.toLower();
    normalized.remove(QLatin1Char('-'));
    normalized.remove(QLatin1Char('_'));
    return sensitiveKeys().contains(normalized);
}

// logs-page-logs.ui.tsx:259: `${level} ${scope} ${runtime} ${message}
// ${JSON.stringify(data)}` lower-cased. Qt compact JSON matches the JS
// serialization shape (key order differs inside objects, same tokens).
QString searchableSessionLogText(const SessionLogEntry &entry)
{
    QString serializedData;
    if (entry.hasData) {
        serializedData = QString::fromUtf8(
            QJsonDocument::fromVariant(entry.data).toJson(QJsonDocument::Compact));
        while (serializedData.endsWith(QLatin1Char('\n')) || serializedData.endsWith(QLatin1Char('\r')))
            serializedData.chop(1);
    }
    return entry.level + QLatin1Char(' ') + entry.scope + QLatin1Char(' ') + entry.runtime
        + QLatin1Char(' ') + entry.message + QLatin1Char(' ') + serializedData;
}

// QJsonDocument::Indented indents with 4 spaces; JSON.stringify(x, null, 2)
// uses 2. Collapse the leading whitespace of every line (JSON strings never
// contain raw newlines, so line starts are always structural indentation).
QString collapseJsonIndent(const QByteArray &text)
{
    const auto lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    QString out;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const auto &line = lines.at(index);
        qsizetype width = 0;
        while (width < line.size() && line.at(width) == QLatin1Char(' '))
            ++width;
        out += QString((width / 4) * 2, QLatin1Char(' ')) + line.mid(width);
        if (index + 1 < lines.size())
            out += QLatin1Char('\n');
    }
    return out;
}

} // namespace

QVariant redactLogValue(const QVariant &value, int depth)
{
    if (depth > 8)
        return QStringLiteral("[max depth]");
    if (!value.isValid() || value.isNull())
        return value;
    switch (value.typeId()) {
    case QMetaType::QString:
        return isSensitiveValue(value.toString()) ? QStringLiteral("[REDACTED]") : value;
    case QMetaType::QVariantMap: {
        QVariantMap out;
        const auto map = value.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            out.insert(it.key(), isSensitiveKey(it.key())
                ? QStringLiteral("[REDACTED]") : redactLogValue(it.value(), depth + 1));
        return out;
    }
    case QMetaType::QVariantList: {
        QVariantList out;
        const auto list = value.toList();
        for (const auto &item : list)
            out.append(redactLogValue(item, depth + 1));
        return out;
    }
    default:
        return value;
    }
}

QString logSourceBucket(const QString &scope)
{
    if (scope == QLatin1String("api"))
        return QStringLiteral("api");
    if (scope == QLatin1String("console"))
        return QStringLiteral("console");
    if (scope.startsWith(QLatin1String("trace:")))
        return QStringLiteral("trace");
    if (scope == QLatin1String("action") || scope == QLatin1String("auth")
        || scope == QLatin1String("realtime") || scope.startsWith(QLatin1String("action:")))
        return QStringLiteral("actions");
    return QStringLiteral("app");
}

bool matchesLogSourceFilter(const QString &scope, const QString &filter)
{
    if (filter == QLatin1String("all"))
        return true;
    return logSourceBucket(scope) == filter;
}

bool logEntryMatchesFilters(const SessionLogEntry &entry, const QString &levelFilter,
    const QString &sourceFilter, const QString &scopeFilter, const QString &runtimeFilter,
    const QString &searchQuery)
{
    if (levelFilter != QLatin1String("all") && entry.level != levelFilter)
        return false;
    if (!matchesLogSourceFilter(entry.scope, sourceFilter))
        return false;
    if (scopeFilter != QLatin1String("all") && entry.scope != scopeFilter)
        return false;
    if (runtimeFilter != QLatin1String("all") && entry.runtime != runtimeFilter)
        return false;
    const auto query = searchQuery.trimmed().toLower();
    if (query.isEmpty())
        return true;
    return searchableSessionLogText(entry).toLower().contains(query);
}

QList<SessionLogEntry> filterSessionLogEntries(const QList<SessionLogEntry> &entries,
    const QString &levelFilter, const QString &sourceFilter, const QString &scopeFilter,
    const QString &runtimeFilter, const QString &searchQuery)
{
    QList<SessionLogEntry> out;
    for (const auto &entry : entries)
        if (logEntryMatchesFilters(entry, levelFilter, sourceFilter, scopeFilter,
                runtimeFilter, searchQuery))
            out.append(entry);
    return out;
}

QString buildLogsExportFilename(qint64 nowUtcMs)
{
    const auto now = QDateTime::fromMSecsSinceEpoch(nowUtcMs, QTimeZone::UTC);
    return QStringLiteral("workspace-logs-%1.json").arg(now.toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

QJsonObject sessionLogEntryJson(const SessionLogEntry &entry)
{
    QJsonObject json;
    json.insert(QStringLiteral("level"), entry.level);
    json.insert(QStringLiteral("scope"), entry.scope);
    json.insert(QStringLiteral("message"), entry.message);
    json.insert(QStringLiteral("timestamp"), entry.timestamp);
    json.insert(QStringLiteral("runtime"), entry.runtime);
    if (entry.hasData)
        json.insert(QStringLiteral("data"),
            QJsonValue::fromVariant(redactLogValue(entry.data)));
    return json;
}

QString serializeLogsForExport(const QList<SessionLogEntry> &entries)
{
    QJsonArray array;
    for (const auto &entry : entries)
        array.append(sessionLogEntryJson(entry));
    return collapseJsonIndent(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

SessionLogBuffer::SessionLogBuffer(QObject *parent)
    : QObject(parent)
    , m_runtimeLabel(nativeRuntimeLabel())
{
}

QString SessionLogBuffer::nativeRuntimeLabel()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WINDOWS)
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

void SessionLogBuffer::setRuntimeLabel(const QString &label)
{
    m_runtimeLabel = label;
}

void SessionLogBuffer::append(const QString &level, const QString &scope,
    const QString &message, const QVariantMap &data, qint64 timestampMs)
{
    SessionLogEntry entry;
    entry.level = level;
    entry.scope = scope;
    entry.message = message;
    const auto stampMs = timestampMs >= 0 ? timestampMs : QDateTime::currentMSecsSinceEpoch();
    entry.timestamp = QDateTime::fromMSecsSinceEpoch(stampMs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
    entry.runtime = m_runtimeLabel;
    // logger.ts redacts data on write so the live UI can never paint secrets.
    if (!data.isEmpty()) {
        entry.data = redactLogValue(data).toMap();
        entry.hasData = true;
    }
    m_entries.prepend(entry); // newest first
    m_scopeEntries = qMin<qsizetype>(MaxEntries, m_scopeEntries + 1);
    while (m_entries.size() > MaxEntries)
        m_entries.removeLast(); // reference evicts the oldest entry
    emit entriesChanged();
}

QList<SessionLogEntry> SessionLogBuffer::entries() const
{
    return m_entries;
}

SessionLogSummary SessionLogBuffer::summary(qint64 nowMs) const
{
    SessionLogSummary result;
    result.totalLines = m_entries.size();
    for (const auto &entry : m_entries) {
        if (entry.level == QLatin1String("debug")) ++result.debugCount;
        else if (entry.level == QLatin1String("info")) ++result.infoCount;
        else if (entry.level == QLatin1String("warn")) ++result.warnCount;
        else if (entry.level == QLatin1String("error")) {
            ++result.errorCount;
            const auto stamp = QDateTime::fromString(entry.timestamp, Qt::ISODateWithMs);
            if (stamp.isValid() && stamp.toMSecsSinceEpoch() >= nowMs - RecentErrorWindowMs) {
                ++result.recentErrorCount;
                if (result.recentErrors.size() < RecentErrorLimit)
                    result.recentErrors.append(entry);
            }
        }
    }
    return result;
}

QVariantMap SessionLogBuffer::supportEventCounts() const
{
    QVariantMap counts{{"debug", 0}, {"info", 0}, {"warn", 0}, {"error", 0}};
    for (qsizetype index = 0; index < m_scopeEntries; ++index) {
        const auto level = m_entries.at(index).level;
        auto found = counts.find(level);
        if (found != counts.end()) found.value() = found.value().toInt() + 1;
    }
    return counts;
}

void SessionLogBuffer::clear()
{
    m_scopeEntries = 0;
    if (m_entries.isEmpty()) {
        emit entriesChanged();
        return;
    }
    m_entries.clear();
    emit entriesChanged();
}

} // namespace workspace::diagnostics

namespace workspace::diagnostics {

// ---------------------------------------------------------------------
// Issue #52: NativeMemoryDiagnostics - the memory panel provider
// (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md, "Memory panel" section). The
// dataset mirrors the Electron diagnostics:getMemorySnapshot shape that feeds
// the reference cards (workspace_ui diagnostics-memory.lib.ts:15-40 ->
// logs-page-memory.ui.tsx:96-197): working set / peak / CPU per process,
// system total/free/available and capability flags. Procfs stays gated on
// Q_OS_LINUX exactly like NativeLogging::memorySnapshot().
// ---------------------------------------------------------------------

namespace {

QByteArray memoryReadProc(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// Parses "Key:   <value> kB" lines of /proc/<pid>/status and /proc/meminfo.
// Reports whether the key parsed at all: a missing or malformed line yields
// std::nullopt, so callers can tell "absent" from a genuine 0 kB reading.
std::optional<qint64> memoryProcKbParsed(const QByteArray &contents, const QByteArray &key)
{
    const QByteArray prefix = key + ':';
    for (const QByteArray &line : contents.split('\n')) {
        if (!line.startsWith(prefix))
            continue;
        const QByteArray value = line.mid(prefix.size()).simplified().split(' ').first();
        bool ok = false;
        const qint64 parsed = value.toLongLong(&ok);
        if (ok && parsed >= 0)
            return parsed;
    }
    return std::nullopt;
}

// Zero-fallback convenience for keys whose card does not gate on the parse
// result (see the memory panel capability flags in snapshot()).
qint64 memoryProcKb(const QByteArray &contents, const QByteArray &key)
{
    return memoryProcKbParsed(contents, key).value_or(0);
}

#if defined(Q_OS_LINUX)
struct CpuSample {
    qint64 ticks;
    qint64 start;
    double uptime;
};

std::optional<CpuSample> readCpuSample(const QString &procRoot)
{
    const QByteArray stat = memoryReadProc(procRoot + QStringLiteral("/self/stat"));
    const int close = stat.lastIndexOf(')');
    if (close < 0)
        return std::nullopt;
    const QList<QByteArray> fields = stat.mid(close + 1).simplified().split(' ');
    if (fields.size() < 20)
        return std::nullopt;
    bool okUser = false;
    bool okSystem = false;
    bool okStart = false;
    const qint64 userTicks = fields.at(11).toLongLong(&okUser);
    const qint64 systemTicks = fields.at(12).toLongLong(&okSystem);
    const qint64 startTicks = fields.at(19).toLongLong(&okStart);
    const QByteArray uptime = memoryReadProc(procRoot + QStringLiteral("/uptime"))
                                  .simplified().split(' ').first();
    bool okUptime = false;
    const double uptimeSeconds = uptime.toDouble(&okUptime);
    if (!okUser || !okSystem || !okStart || !okUptime
        || !std::isfinite(uptimeSeconds) || uptimeSeconds <= 0 || startTicks < 0
        || userTicks < 0 || systemTicks < 0 || userTicks > std::numeric_limits<qint64>::max() - systemTicks)
        return std::nullopt;
    return CpuSample{userTicks + systemTicks, startTicks, uptimeSeconds};
}
#endif

// JavaScript Date.toISOString() equivalent: UTC with milliseconds, Z suffix
// (the same stamp contract as the accepted NativeLogging snapshot).
QString memoryIsoTimestampNow()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"))
        + QLatin1Char('Z');
}

} // namespace

void NativeMemoryDiagnostics::setProcRoot(QString procRoot)
{
    resetCpu();
    m_procRoot = std::move(procRoot);
    m_peakWorkingSetKb = 0; // a new root starts its own peak history
}

void NativeMemoryDiagnostics::resetCpu() const
{
    m_previousUptime = -1;
    m_previousStart = -1;
    m_cpuPercent.clear();
}

QVariantMap NativeMemoryDiagnostics::snapshot(bool sampleCpu) const
{
    Q_UNUSED(sampleCpu);
    qint64 rssKb = 0;
    qint64 totalKb = 0;
    qint64 freeKb = 0;
    qint64 availableKb = 0;
    // exordy review PRRT_kwDOVmpjls65Qz-z: the free-memory card may only
    // render when the MemFree line actually parsed, never off the 0 fallback.
    bool freeParsed = false;
    QVariantList processes;
#if defined(Q_OS_LINUX)
    const QByteArray status = memoryReadProc(m_procRoot + QStringLiteral("/self/status"));
    rssKb = memoryProcKb(status, "VmRSS");
    const qint64 peakKb = memoryProcKb(status, "VmHWM");
    const QByteArray meminfo = memoryReadProc(m_procRoot + QStringLiteral("/meminfo"));
    totalKb = memoryProcKb(meminfo, "MemTotal");
    const auto freeKbOpt = memoryProcKbParsed(meminfo, "MemFree");
    freeParsed = freeKbOpt.has_value();
    freeKb = freeKbOpt.value_or(0);
    availableKb = memoryProcKb(meminfo, "MemAvailable");
    // Monotonic peak: kernel VmHWM plus the cross-poll clamp, so a regressing
    // fixture peak can never lower the reported value.
    m_peakWorkingSetKb = qMax(m_peakWorkingSetKb, qMax(peakKb, rssKb));
    if (sampleCpu) {
        const auto sample = readCpuSample(m_procRoot);
        const long hz = sysconf(_SC_CLK_TCK);
        if (rssKb <= 0 || !sample || hz <= 0) {
            resetCpu();
        } else {
            const double elapsed = sample->uptime - m_previousUptime;
            if (m_previousStart != sample->start || elapsed < 0 || sample->ticks < m_previousTicks) {
                m_cpuPercent.clear();
            } else if (elapsed > 0) {
                m_cpuPercent = 100.0 * double(sample->ticks - m_previousTicks) / double(hz) / elapsed;
            }
            // Two calls in the same procfs clock tick do not start a new interval.
            if (elapsed != 0 || m_previousStart != sample->start || sample->ticks < m_previousTicks) {
                m_previousUptime = sample->uptime;
                m_previousStart = sample->start;
                m_previousTicks = sample->ticks;
            }
        }
    }
    if (rssKb > 0) {
        QVariantMap self;
        self.insert(QStringLiteral("type"), QStringLiteral("Main"));
        self.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
        self.insert(QStringLiteral("workingSetKb"), rssKb);
        self.insert(QStringLiteral("peakWorkingSetKb"), m_peakWorkingSetKb);
        self.insert(QStringLiteral("cpuPercent"), m_cpuPercent);
        processes.append(self);
    }
#endif
    QVariantMap app;
    app.insert(QStringLiteral("workingSetKb"), rssKb);
    app.insert(QStringLiteral("peakWorkingSetKb"), m_peakWorkingSetKb);
    app.insert(QStringLiteral("totalWorkingSetKb"), rssKb);
    QVariantMap system;
    system.insert(QStringLiteral("totalKb"), totalKb);
    system.insert(QStringLiteral("freeKb"), freeKb);
    system.insert(QStringLiteral("availableKb"), availableKb);
    QVariantMap capabilities;
    capabilities.insert(QStringLiteral("processMetricsAvailable"), !processes.isEmpty());
    capabilities.insert(QStringLiteral("systemMemoryAvailable"),
        totalKb > 0 && availableKb > 0 && availableKb <= totalKb);
    capabilities.insert(QStringLiteral("systemMemoryFreeAvailable"), freeParsed);
    QVariantMap snapshot;
    snapshot.insert(QStringLiteral("collectedAt"), memoryIsoTimestampNow());
    snapshot.insert(QStringLiteral("runtime"), QStringLiteral("native"));
    snapshot.insert(QStringLiteral("system"), system);
    snapshot.insert(QStringLiteral("app"), app);
    snapshot.insert(QStringLiteral("processes"), processes);
    snapshot.insert(QStringLiteral("capabilities"), capabilities);
    return snapshot;
}

} // namespace workspace::diagnostics
