#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace workspace {

// Persistent rotating diagnostics log plus the process/system memory snapshot
// provider for the future Diagnostics pages (issue #77,
// docs/NATIVE_DESKTOP_DIAGNOSTICS.md). Mirrors the measured reference contract
// in workspace_ui main.ts:112-182 and main.ts:1228-1267 (sha 61dacef7).
class NativeLogging final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaxLogFileBytes = 1024 * 1024;
    static constexpr qsizetype MaxLogLineLength = 32768;

    // rootDir is the userData-style root the workspace_logs directory lives
    // under; production leaves it empty (AppDataLocation), tests inject a
    // temporary directory so no developer paths are touched.
    explicit NativeLogging(QString rootDir = {}, QObject *parent = nullptr);

    QString logDirectoryPath() const;
    QString filePath() const;
    QString rotatedFilePath() const;
    void setProcRoot(QString procRoot);

    // Ensures the log directory exists and returns the absolute log file path
    // (empty on failure) — the native logs:getFilePath equivalent.
    Q_INVOKABLE QString getFilePath();

    // Native logs:append equivalent: strips control characters, truncates to
    // MaxLogLineLength, rejects empty-after-sanitize lines, rotates at
    // MaxLogFileBytes and appends "<ISO-8601 UTC> <line>\n". False on empty
    // lines or any I/O error.
    Q_INVOKABLE bool appendLogLine(const QString &rawLine);

    // diagnostics:getMemorySnapshot equivalent (single-process shape; see the
    // design doc for the recorded deviations and unit contract).
    Q_INVOKABLE QVariantMap memorySnapshot() const;

    // Qt message handler line rendering: "<level>: <message>".
    static QString formatMessageLine(QtMsgType type, const QString &message);

private:
    bool ensureLogDirectory();
    void rotateLogsIfNeeded();

    QString m_rootDir;
    QString m_procRoot = QStringLiteral("/proc");
    QMutex m_ioMutex;
};

// Routes qDebug/qInfo/qWarning/qCritical lines into the log file while
// keeping the default stderr console behavior. removeNativeLogHandler()
// restores Qt's default handling.
void installNativeLogHandler(NativeLogging &logging);
void removeNativeLogHandler();

} // namespace workspace
