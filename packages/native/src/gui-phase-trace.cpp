#include "gui-phase-trace.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <cstdlib>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <atomic>
#include <vector>

namespace {

struct OpenSpan
{
    qint64 id = 0;
    QString name;
    QString parent;
    qint64 threadId = 0;
    QElapsedTimer timer;
};

struct FinishedSpan
{
    QString name;
    QString parent;
    qint64 threadId = 0;
    double ms = 0.0;
    int depth = 0;
};

QMutex g_traceMutex;
std::vector<FinishedSpan> g_finished;       // bounded FIFO of finished spans
QHash<QString, GuiPhaseTrace::Aggregate> g_aggregates;
qint64 g_nextSpanId = 0;
qint64 g_guiThreadId = 0;                   // thread that ran the outermost first span

thread_local std::vector<OpenSpan> g_threadStack;

void closeTop(OpenSpan &&top)
{
    FinishedSpan done;
    done.name = top.name;
    done.parent = top.parent;
    done.threadId = top.threadId;
    done.ms = double(top.timer.nsecsElapsed()) / 1.0e6;
    done.depth = int(g_threadStack.size()); // stack already popped by caller
    QMutexLocker lock(&g_traceMutex);
    auto &agg = g_aggregates[top.name];
    agg.count += 1;
    agg.totalMs += done.ms;
    agg.maxMs = qMax(agg.maxMs, done.ms);
    if (g_finished.size() >= GuiPhaseTrace::kRecordCap)
        g_finished.erase(g_finished.begin());
    g_finished.push_back(std::move(done));
}

} // namespace

GuiPhaseTrace &GuiPhaseTrace::instance()
{
    static GuiPhaseTrace trace;
    return trace;
}

void GuiPhaseTrace::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

void GuiPhaseTrace::reset()
{
    {
        QMutexLocker lock(&g_traceMutex);
        g_aggregates.clear();
        g_finished.clear();
        g_nextSpanId = 0;
        g_guiThreadId = 0;
    }
    g_threadStack.clear();
    m_enabled = true;
}

QVariantMap GuiPhaseTrace::snapshot() const
{
    QMutexLocker lock(&g_traceMutex);
    QVariantMap spans;
    for (auto it = g_aggregates.constBegin(); it != g_aggregates.constEnd(); ++it)
        spans.insert(it.key(), QVariantMap{{QStringLiteral("count"), it->count},
                                           {QStringLiteral("totalMs"), it->totalMs},
                                           {QStringLiteral("maxMs"), it->maxMs}});
    QVariantList records;
    for (const auto &done : g_finished)
        records.append(QVariantMap{{QStringLiteral("name"), done.name},
                                   {QStringLiteral("parent"), done.parent},
                                   {QStringLiteral("threadId"), done.threadId},
                                   {QStringLiteral("ms"), done.ms},
                                   {QStringLiteral("depth"), done.depth}});
    return QVariantMap{{QStringLiteral("enabled"), isEnabled()},
                       {QStringLiteral("guiThreadId"), g_guiThreadId},
                       {QStringLiteral("currentThreadId"), qint64(quintptr(QThread::currentThreadId()))},
                       {QStringLiteral("spans"), spans},
                       {QStringLiteral("records"), records}};
}

qint64 GuiPhaseTrace::beginSpan(const QString &name)
{
    if (!m_enabled) return 0;
    OpenSpan span;
    span.id = ++g_nextSpanId;
    span.name = name;
    span.threadId = qint64(quintptr(QThread::currentThreadId()));
    span.parent = g_threadStack.empty() ? QString() : g_threadStack.back().name;
    span.timer.start();
    {
        QMutexLocker lock(&g_traceMutex);
        if (g_guiThreadId == 0) g_guiThreadId = span.threadId;
    }
    g_threadStack.push_back(std::move(span));
    return g_threadStack.back().id;
}

void GuiPhaseTrace::endSpan(qint64 id)
{
    if (!m_enabled || id == 0) return;
    if (g_threadStack.empty()) return;
    // Defensive: close inner spans down to the requested id if a caller
    // unwound out of order (early returns inside instrumented scopes).
    for (int i = int(g_threadStack.size()) - 1; i >= 0; --i)
    {
        OpenSpan top = std::move(g_threadStack.back());
        g_threadStack.pop_back();
        closeTop(std::move(top));
        if (top.id == id) break;
    }
}

GuiPhaseSpan::GuiPhaseSpan(const char *name)
{
    // Inert-telemetry contract: a disabled recorder must not convert the span
    // name at all - the disabled path pays one relaxed bool read and nothing
    // else (exordy finding on PR #898). beginSpan keeps its own check for the
    // QML beginSpan seam, which receives an already-converted QString.
    if (!GuiPhaseTrace::instance().isEnabled()) return;
    m_id = GuiPhaseTrace::instance().beginSpan(QString::fromLatin1(name));
}

GuiPhaseSpan::~GuiPhaseSpan()
{
    GuiPhaseTrace::instance().endSpan(m_id);
}

namespace {
constexpr int kStallIntervalMs = 10;
constexpr int kStallFlushMs = 500;
}

GuiStallSampler::GuiStallSampler(const QString &reportPath, QObject *parent)
    : QObject(parent), m_path(reportPath)
{
    connect(&m_beat, &QTimer::timeout, this, [this] { beat(); });
    connect(&m_flush, &QTimer::timeout, this, [this] { flush(); });
    m_beat.setInterval(kStallIntervalMs);
    m_flush.setInterval(kStallFlushMs);
    m_sinceBeat.start();
    m_beat.start();
    m_flush.start();
}

GuiStallSampler *GuiStallSampler::startFromEnvironment(QObject *parent)
{
    const QByteArray path = qgetenv("WORKSPACE_NATIVE_STALL_PROBE");
    if (path.isEmpty()) return nullptr;
    return new GuiStallSampler(QString::fromLocal8Bit(path), parent);
}

void GuiStallSampler::beat()
{
    // QElapsedTimer::restart() returns ELAPSED MILLISECONDS, not nanoseconds:
    // dividing it by 1e6 (first smoke run) silently floored every gap to 0.0
    // and froze the report's worst/mean at zero. Read nanoseconds explicitly,
    // then re-arm the clock.
    const double gapMs = double(m_sinceBeat.nsecsElapsed()) / 1.0e6;
    m_sinceBeat.restart();
    // Every gap counts, the first one included: the constructor started the
    // clock, so gap #1 covers startup work that blocked the GUI thread
    // before the event loop ever delivered a beat. Excluding it hid exactly
    // the stalls this sampler exists to measure.
    m_worstGapMs = qMax(m_worstGapMs, gapMs);
    m_totalGapMs += gapMs;
    if (gapMs > 50.0) ++m_over50Ms;
    if (gapMs > 300.0) ++m_over300Ms;
    ++m_samples;
}

void GuiStallSampler::flush() const
{
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    const QVariantMap report = summary();
    file.write(QJsonDocument(QJsonObject::fromVariantMap(report)).toJson(QJsonDocument::Compact));
}

QVariantMap GuiStallSampler::summary() const
{
    return QVariantMap{
        {QStringLiteral("interval_ms"), kStallIntervalMs},
        {QStringLiteral("samples"), m_samples},
        {QStringLiteral("worst_gap_ms"), qRound(m_worstGapMs * 10.0) / 10.0},
        {QStringLiteral("mean_gap_ms"), m_samples > 0
            ? qRound((m_totalGapMs / m_samples) * 10.0) / 10.0 : 0.0},
        {QStringLiteral("beats_over_50ms"), m_over50Ms},
        {QStringLiteral("beats_over_300ms"), m_over300Ms},
    };
}
