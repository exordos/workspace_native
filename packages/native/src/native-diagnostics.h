#pragma once

#include <QDateTime>
#include <QHash>
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

struct SessionLogSummary {
    int totalLines = 0;
    int debugCount = 0;
    int infoCount = 0;
    int warnCount = 0;
    int errorCount = 0;
    int recentErrorCount = 0;
    QList<SessionLogEntry> recentErrors; // newest first, at most RecentErrorLimit
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
    SessionLogSummary summary(qint64 nowMs) const;
    void beginSupportScope() { m_scopeEntries = 0; }
    QVariantMap supportEventCounts() const;
    int size() const { return m_entries.size(); }
    void clear();

signals:
    void entriesChanged();

private:
    qsizetype m_scopeEntries = 0;
    QList<SessionLogEntry> m_entries; // newest first
    QString m_runtimeLabel;
};

// Issue #52: the small procfs memory provider feeding the diagnostics memory
// panel (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md, "Memory panel" section).
// Procfs reads stay gated on Q_OS_LINUX exactly like
// NativeLogging::memorySnapshot(); non-Linux builds keep the same key set
// with zeros and both capability flags false, which the panel renders as the
// unavailable state. The reported peak working set is additionally clamped
// monotonic across polls so a regressing VmHWM can never lower it.
// Malformed or missing sources fall back to zeros, an empty processes list
// and capability flags false (the unavailable fallback).
class NativeMemoryDiagnostics final {
public:
    // Injects the fixture proc root (test seam mirroring
    // NativeLogging::setProcRoot); resets the monotonic peak clamp so a new
    // root always starts its own peak history.
    void setProcRoot(QString procRoot);
    QString procRoot() const { return m_procRoot; }

    // Collects one snapshot: { collectedAt, runtime: "native",
    // system{totalKb, freeKb, availableKb}, app{workingSetKb,
    // peakWorkingSetKb, totalWorkingSetKb}, processes[{type "Main", pid,
    // workingSetKb, peakWorkingSetKb, cpuPercent|null}],
    // capabilities{processMetricsAvailable, systemMemoryAvailable} }.
    QVariantMap snapshot(bool sampleCpu = true) const;
    void resetCpu() const;

private:
    QString m_procRoot = QStringLiteral("/proc");
    // /proc/uptime is monotonic and shares its time base with process start ticks.
    mutable double m_previousUptime = -1;
    mutable qint64 m_previousTicks = 0;
    mutable qint64 m_previousStart = -1;
    mutable QVariant m_cpuPercent;
    mutable qint64 m_peakWorkingSetKb = 0; // monotonic across polls
};

// Issue #668 (docs/NATIVE_DESKTOP_WS_DIAGNOSTICS.md): transport-level
// WebSocket accounting for the Server Diagnostics websocket dataset.
// A value class fed from RealtimeClient's existing seams (setState, the
// socket disconnected/errorOccurred handlers, dispatch). Records only
// metadata: state transitions, close codes, event type strings and
// timestamps - never payload bodies, tokens or URLs with credentials.
// Not thread-safe; callers run on a single thread.
class RealtimeActivityLedger final {
public:
    static constexpr int HistoryCap = 50;
    static constexpr int FailureCap = 20;
    static constexpr int RecentEventWindowMs = 60'000;
    static constexpr int RecentEventCap = 300;

    // Mirrors every RealtimeClient::setState transition. "live" is the
    // connected sentinel: entering it stamps connectedSince/lastConnected;
    // leaving it ends the span WITHOUT stamping a disconnect (the
    // live<->catching_up oscillation is not a drop). The disconnect stamp is
    // written by recordFailure, which only fires on real socket close/error.
    void recordStateChange(const QString &state, qint64 atMs);
    // reason is a fixed token ("socket_closed", "socket_error"); closeCode
    // is the WebSocket close code or a QAbstractSocket::SocketError value.
    void recordFailure(qint64 atMs, const QString &reason, int closeCode);
    void recordReceived(const QString &eventType, qint64 atMs);

    // The dataset documented in docs/NATIVE_DESKTOP_WS_DIAGNOSTICS.md,
    // minus endpoint/state (the realtime client injects those, it owns the
    // transport configuration). newestFirst applies to both history lists.
    QVariantMap summary(qint64 nowMs) const;

    QString lastState() const { return m_lastState; }
    qint64 connectedSinceMs() const { return m_connectedSinceMs; }

private:
    QString m_lastState;
    qint64 m_connectedSinceMs = 0;
    qint64 m_lastConnectedAtMs = 0;
    qint64 m_lastDisconnectedAtMs = 0;
    qint64 m_lastEventAtMs = 0;
    QString m_lastEventType;
    quint64 m_eventsReceived = 0;
    QHash<QString, quint64> m_byType;
    QList<qint64> m_recentEventTimes;             // oldest first, cap tail
    QList<QVariantMap> m_failures;               // newest first, cap head
    QList<QVariantMap> m_history;                // newest first, cap head
};

} // namespace workspace::diagnostics
