#include "native-diagnostics.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>

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
    while (m_entries.size() > MaxEntries)
        m_entries.removeLast(); // reference evicts the oldest entry
    emit entriesChanged();
}

QList<SessionLogEntry> SessionLogBuffer::entries() const
{
    return m_entries;
}

void SessionLogBuffer::clear()
{
    if (m_entries.isEmpty()) {
        emit entriesChanged();
        return;
    }
    m_entries.clear();
    emit entriesChanged();
}

} // namespace workspace::diagnostics
