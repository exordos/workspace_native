#pragma once

// Issue #893 (#855 subtask 1): GUI-thread phase attribution.
//
// A disabled-by-default recorder of nested GUI-thread time spans for the
// documented startup/navigation phases. Telemetry is inert unless explicitly
// enabled through the test seam (NativeController::resetGuiPhaseTraceForTest),
// so user runs pay one relaxed atomic load per span and nothing else.

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QByteArray>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

class GuiPhaseTrace
{
public:
    static GuiPhaseTrace &instance();

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Begin/end pair usable from QML through the controller seam. Returns a
    // positive span id, or 0 when disabled (end with id 0 is a no-op).
    qint64 beginSpan(const QString &name);
    void endSpan(qint64 id);

    struct Aggregate
    {
        qint64 count = 0;
        double totalMs = 0.0;
        double maxMs = 0.0;
    };

    // Reset clears every recorded span and (re-)enables recording.
    void reset();
    // Snapshot reports per-name aggregates and the bounded record of finished
    // spans with parent attribution, plus the thread that ran the spans.
    QVariantMap snapshot() const;

    static const int kRecordCap = 256;

private:
    bool m_enabled = false;
};

// RAII helper for C++ call sites: names the span and measures the scope.
class GuiPhaseSpan
{
public:
    explicit GuiPhaseSpan(const char *name);
    ~GuiPhaseSpan();
    GuiPhaseSpan(const GuiPhaseSpan &) = delete;
    GuiPhaseSpan &operator=(const GuiPhaseSpan &) = delete;

private:
    qint64 m_id = 0;
};

#define GUI_PHASE_SPAN_HERE(unique, name) GuiPhaseSpan unique(name)

// Issue #855: in-process main-thread stall sampler. A 10 ms repeating timer
// measures the gap between firings; a gap larger than the interval is time the
// GUI thread could not service its event loop. Enabled only when the
// WORKSPACE_NATIVE_STALL_PROBE environment variable names a JSON file to
// receive periodic flushes (packaged-binary use) - completely inert otherwise.
// The same object serves window tests through the controller seam.
class GuiStallSampler : public QObject
{
public:
    explicit GuiStallSampler(const QString &reportPath, QObject *parent = nullptr);
    // Creates and starts a sampler when WORKSPACE_NATIVE_STALL_PROBE is set.
    static GuiStallSampler *startFromEnvironment(QObject *parent);
    QVariantMap summary() const;

    // Test-only fault seam for the atomic-replacement contract (PR #898
    // review): the next flush's payload is cut to its first byteCount bytes
    // and the write is then treated as failed - the deterministic stand-in
    // for ENOSPC or a crash mid-write. The destination must never expose a
    // partial or empty report to a live reader.
    void failNextFlushWriteForTest(int byteCount) { m_failFlushBytes = byteCount; }
    bool flushFaultConsumedForTest() const { return m_flushFaultConsumed; }

private:
    void beat();
    void flush() const;

    QString m_path;
    mutable int m_failFlushBytes = -1;
    mutable bool m_flushFaultConsumed = false;
    QTimer m_beat;
    QTimer m_flush;
    QElapsedTimer m_sinceBeat;
    qint64 m_samples = 0;
    double m_worstGapMs = 0.0;
    double m_totalGapMs = 0.0;
    qint64 m_over50Ms = 0;
    qint64 m_over300Ms = 0;
};
