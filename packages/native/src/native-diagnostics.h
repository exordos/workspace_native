#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace workspace::diagnostics {

// Pure collector core for the Diagnostics page (issue #50,
// docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md). Mirrors the reference math in
// workspace_ui diagnostics-collect.lib.ts:153-188 (status rule, recent-error
// window/cap) and diagnostics-api-latency.lib.ts:20-24 (percentiles). No I/O
// and no state here: the controller feeds live inputs, tests seed fixtures.

inline constexpr int RecentErrorLimit = 8;
inline constexpr qint64 RecentErrorWindowMs = 15LL * 60LL * 1000LL;

// Maps the native connection phase (connectionPhase(): offline | rate_limited
// | reconnecting | degraded | online) plus reachability and the recent-error
// count into the dashboard status "healthy" | "degraded" | "offline". The
// native "reconnecting" phase plays the reference "connecting" role;
// "blocked" is kept for the reference shape even though no native producer
// emits it yet.
inline QString resolveOverallStatus(const QString &phase, bool online, int recentErrorCount)
{
    if (phase == QLatin1String("offline") || !online)
        return QStringLiteral("offline");
    if (phase == QLatin1String("degraded") || phase == QLatin1String("blocked")
        || phase == QLatin1String("rate_limited") || phase == QLatin1String("connecting")
        || phase == QLatin1String("reconnecting") || recentErrorCount > 0)
        return QStringLiteral("degraded");
    return QStringLiteral("healthy");
}

struct RecentLogError {
    QString timestamp; // original ISO-8601 UTC stamp from the log line
    QString level;     // "critical" | "fatal"
    QString message;
};

struct SessionLogSummary {
    int totalLines = 0;
    int debugCount = 0;
    int infoCount = 0;
    int warnCount = 0;
    int errorCount = 0;
    int recentErrorCount = 0;
    QList<RecentLogError> recentErrors; // newest first, at most RecentErrorLimit
};

// Parses the NativeLogging line format "<ISO-8601 UTC> <level>: <message>"
// with level names debug|info|warning|critical|fatal
// (docs/NATIVE_DESKTOP_DIAGNOSTICS.md). Level buckets reproduce the reference
// byLevel shape: warning counts as warn, critical and fatal count as error.
// Recent errors are error-bucket lines whose timestamp falls inside the last
// RecentErrorWindowMs; the list is newest first and capped at
// RecentErrorLimit while recentErrorCount stays uncapped.
inline SessionLogSummary parseSessionLog(const QString &logText, qint64 nowMs)
{
    SessionLogSummary summary;
    const auto lines = logText.split(QLatin1Char('\n'));
    QList<RecentLogError> collected;
    for (const auto &line : lines) {
        const auto trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        const auto space = trimmed.indexOf(QLatin1Char(' '));
        if (space <= 0)
            continue;
        auto stamp = trimmed.left(space);
        // Keep the UTC suffix: chopping it made Qt parse the stamp in the
        // machine-local zone (review 4611502317), shifting recent-error
        // decisions by the local offset.
        auto when = QDateTime::fromString(stamp, Qt::ISODateWithMs);
        if (!when.isValid())
            when = QDateTime::fromString(stamp, Qt::ISODate);
        if (!when.isValid())
            continue;
        ++summary.totalLines;
        const auto rest = trimmed.mid(space + 1);
        const auto colon = rest.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const auto level = rest.left(colon).toLower();
        const auto message = rest.mid(colon + 1).trimmed();
        const auto stampMs = when.toUTC().toMSecsSinceEpoch();
        if (level == QLatin1String("debug"))
            ++summary.debugCount;
        else if (level == QLatin1String("info"))
            ++summary.infoCount;
        else if (level == QLatin1String("warning"))
            ++summary.warnCount;
        else if (level == QLatin1String("critical") || level == QLatin1String("fatal")) {
            ++summary.errorCount;
            if (stampMs >= nowMs - RecentErrorWindowMs) {
                ++summary.recentErrorCount;
                collected.prepend(RecentLogError{trimmed.left(space), level, message});
            }
        }
    }
    while (collected.size() > RecentErrorLimit)
        collected.removeLast();
    summary.recentErrors = collected;
    return summary;
}

struct LatencySummary {
    int sampleCount = 0;
    qint64 medianMs = -1; // -1 means "no samples" (reference null)
    qint64 p95Ms = -1;
};

// percentile(sorted, p) = sorted[min(n-1, floor(n*p))] — the exact reference
// selection (diagnostics-api-latency.lib.ts:20-24), median at 0.5, p95 at 0.95.
inline LatencySummary summarizeLatency(const QList<qint64> &samples)
{
    LatencySummary summary;
    summary.sampleCount = samples.size();
    if (samples.isEmpty())
        return summary;
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto pick = [&sorted](double p) {
        const auto index = qMin(sorted.size() - 1,
            static_cast<qsizetype>(std::floor(static_cast<double>(sorted.size()) * p)));
        return sorted.at(index);
    };
    summary.medianMs = pick(0.5);
    summary.p95Ms = pick(0.95);
    return summary;
}

// ---- Issue #51: in-memory session log for the log viewer modal -----------
// (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md, modal section). Mirrors the
// reference history surface in workspace_ui shared/lib/logger.ts: entry
// shape, redact(), the source-bucket filter (logs-source-filter.lib.ts),
// the filter semantics (logs-page-logs.ui.tsx:248-268) and the export
// format (logs-export.lib.ts).

struct SessionLogEntry {
    QString level;     // debug | info | warn | error (reference LogLevel)
    QString scope;
    QString message;
    QString timestamp; // ISO-8601 UTC with ms (JS Date.toISOString shape)
    QString runtime;   // native runtime label (fixed set: linux | macos | windows)
    QVariantMap data;  // optional structured payload
    bool hasData = false;
};

// logger.ts redact(): sensitive keys are lower-cased with '-'/'_' stripped
// before lookup; string values matching the token patterns and maps deeper
// than depth 8 are replaced.
QVariant redactLogValue(const QVariant &value, int depth = 0);

// logs-source-filter.lib.ts:3-32: api = scope "api"; console = scope
// "console"; trace = scope prefix "trace:"; actions = scope in {action,
// auth, realtime} or prefix "action:"; app = everything else.
QString logSourceBucket(const QString &scope);
bool matchesLogSourceFilter(const QString &scope, const QString &filter);

// filterLogEntries semantics (logs-page-logs.ui.tsx:248-268): level, source,
// scope and runtime AND together ("all" passes); the search is a trimmed
// case-insensitive substring of "level scope runtime message JSON(data)".
bool logEntryMatchesFilters(const SessionLogEntry &entry, const QString &levelFilter,
    const QString &sourceFilter, const QString &scopeFilter, const QString &runtimeFilter,
    const QString &searchQuery);
QList<SessionLogEntry> filterSessionLogEntries(const QList<SessionLogEntry> &entries,
    const QString &levelFilter, const QString &sourceFilter, const QString &scopeFilter,
    const QString &runtimeFilter, const QString &searchQuery);

// logs-export.lib.ts: UTC "workspace-YYYYMMDD-HHMMSS.json" name and the
// 2-space pretty JSON array with every entry's data re-run through redact().
QString buildLogsExportFilename(qint64 nowUtcMs);
QJsonObject sessionLogEntryJson(const SessionLogEntry &entry);
QString serializeLogsForExport(const QList<SessionLogEntry> &entries);

// The logger ring buffer (shared/lib/logger.ts pushToBuffer/getLogHistory/
// clearLogHistory/subscribeLogHistory): 500 entries max, oldest evicted,
// newest-first reads, one entriesChanged() notification per append and clear.
class SessionLogBuffer final : public QObject {
    Q_OBJECT
public:
    static inline constexpr int MaxEntries = 500;

    explicit SessionLogBuffer(QObject *parent = nullptr);

    // The fixed native runtime value set (docs modal section, deviation 1):
    // linux | macos | windows, stamped onto every entry like the reference
    // getRuntime() stamp.
    static QString nativeRuntimeLabel();
    void setRuntimeLabel(const QString &label);
    QString runtimeLabel() const { return m_runtimeLabel; }

    void append(const QString &level, const QString &scope, const QString &message,
        const QVariantMap &data = QVariantMap(), qint64 timestampMs = -1);
    QList<SessionLogEntry> entries() const; // newest first
    int size() const { return m_entries.size(); }
    void clear();

signals:
    void entriesChanged();

private:
    QList<SessionLogEntry> m_entries; // newest first
    QString m_runtimeLabel;
};

} // namespace workspace::diagnostics
