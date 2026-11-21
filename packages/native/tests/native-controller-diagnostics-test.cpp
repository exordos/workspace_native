#include "native-logging.h"
#include "support/native-test-harness.h"
#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/resource.h>
#endif

class ControllerDiagnosticsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
private:
    static bool seedDiagnosticsLog(const QString &path, int bytes, char filler)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(QByteArray(bytes, filler));
        return file.error() == QFileDevice::NoError;
    }

    static QString readDiagnosticsLog(const QString &path, bool *ok = nullptr)
    {
        QFile file(path);
        const bool opened = file.open(QIODevice::ReadOnly);
        if (ok) *ok = opened;
        return opened ? QString::fromUtf8(file.readAll()) : QString();
    }

private slots:
    // Diagnostics providers (issue #77, docs/NATIVE_DESKTOP_DIAGNOSTICS.md):
    // measured reference contract from workspace_ui main.ts:112-182/1228-1267.
    void diagnosticsLogPathLayoutUnderInjectedRoot()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        const QString dir = root.path() + QStringLiteral("/workspace_logs");
        QCOMPARE(logs.logDirectoryPath(), dir);
        QCOMPARE(logs.filePath(), dir + QStringLiteral("/workspace.log"));
        QCOMPARE(logs.rotatedFilePath(), dir + QStringLiteral("/workspace.log.1"));
        QVERIFY(!QFileInfo::exists(dir));
        QCOMPARE(logs.getFilePath(), dir + QStringLiteral("/workspace.log"));
        QVERIFY2(QFileInfo::exists(dir), "getFilePath must ensure the log directory");
        QVERIFY(logs.appendLogLine(QStringLiteral("first")));
        const QString text = readDiagnosticsLog(logs.filePath());
        QVERIFY2(text.endsWith(QStringLiteral("first\n")), qPrintable(text));
    }

    void diagnosticsLogLineUsesIsoPrefixAndNewlineTerminatedLines()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        QVERIFY(logs.appendLogLine(QStringLiteral("hello world")));
        QString text = readDiagnosticsLog(logs.filePath());
        QVERIFY2(text.endsWith(QLatin1Char('\n')), "lines must be newline-terminated");
        const QRegularExpression isoLine(
            QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z hello world\\n$"));
        QVERIFY2(isoLine.match(text).hasMatch(),
                 qPrintable(QStringLiteral("unexpected log line: %1").arg(text)));
        // Empty-after-sanitize lines are rejected and write nothing.
        QVERIFY(!logs.appendLogLine(QString()));
        QVERIFY(!logs.appendLogLine(QStringLiteral("   \t ")));
        QVERIFY(!logs.appendLogLine(QString(QChar(0x07))));
        text = readDiagnosticsLog(logs.filePath());
        QCOMPARE(text.count(QLatin1Char('\n')), 1);
        // Every appended line persists: 40 appends leave 40 timestamped lines.
        for (int i = 0; i < 40; ++i)
            QVERIFY(logs.appendLogLine(QStringLiteral("line-%1").arg(i)));
        text = readDiagnosticsLog(logs.filePath());
        QCOMPARE(text.count(QLatin1Char('\n')), 41);
        QVERIFY(text.contains(QStringLiteral("line-39")));
    }

    void diagnosticsLogStripsControlChars()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        QString noisy = QStringLiteral("bell");
        noisy += QChar(0x07);
        noisy += QStringLiteral(" esc");
        noisy += QChar(0x1B);
        noisy += QStringLiteral(" nul");
        noisy += QChar(0x00);
        noisy += QStringLiteral(" del");
        noisy += QChar(0x7F);
        noisy += QStringLiteral(" keep\ttab");
        QVERIFY(logs.appendLogLine(noisy));
        QString text = readDiagnosticsLog(logs.filePath());
        text = text.section(QLatin1Char(' '), 1);
        text.chop(1); // trailing newline
        QCOMPARE(text, QStringLiteral("bell esc nul del keep\ttab"));
    }

    void diagnosticsLogTruncatesTo32768Chars()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        QVERIFY(logs.appendLogLine(QString(40000, QLatin1Char('a'))));
        QString text = readDiagnosticsLog(logs.filePath());
        text = text.section(QLatin1Char(' '), 1);
        text.chop(1); // trailing newline
        QCOMPARE(text.size(), qsizetype(NativeLogging::MaxLogLineLength));
        QVERIFY(text == QString(int(NativeLogging::MaxLogLineLength), QLatin1Char('a')));
    }

    void diagnosticsLogRotatesAtExactlyOneMegabyte()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        QVERIFY(!logs.getFilePath().isEmpty());
        const qint64 boundary = NativeLogging::MaxLogFileBytes;
        QCOMPARE(boundary, qint64(1048576));
        // 1 MiB minus one byte: the append stays in place, no rotation yet.
        QVERIFY(seedDiagnosticsLog(logs.filePath(), int(boundary - 1), 'x'));
        QVERIFY(logs.appendLogLine(QStringLiteral("under-boundary")));
        QVERIFY(!QFileInfo::exists(logs.rotatedFilePath()));
        // Exactly 1 MiB: the next append rotates workspace.log to .1 first.
        QVERIFY(seedDiagnosticsLog(logs.filePath(), int(boundary), 'x'));
        QVERIFY(logs.appendLogLine(QStringLiteral("at-boundary")));
        QVERIFY(QFileInfo::exists(logs.rotatedFilePath()));
        QCOMPARE(QFileInfo(logs.rotatedFilePath()).size(), boundary);
        const QString text = readDiagnosticsLog(logs.filePath());
        QVERIFY2(!text.contains(QLatin1Char('x')), "rotated content must leave workspace.log");
        QVERIFY2(text.endsWith(QStringLiteral("at-boundary\n")), qPrintable(text));
    }

    void diagnosticsLogRotatedFileOverwrittenNotDuplicated()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        QVERIFY(!logs.getFilePath().isEmpty());
        const int boundary = int(NativeLogging::MaxLogFileBytes);
        QVERIFY(seedDiagnosticsLog(logs.filePath(), boundary, 'A'));
        QVERIFY(logs.appendLogLine(QStringLiteral("first")));
        QVERIFY(QFileInfo::exists(logs.rotatedFilePath()));
        QVERIFY(seedDiagnosticsLog(logs.filePath(), boundary, 'B'));
        QVERIFY(logs.appendLogLine(QStringLiteral("second")));
        QDir dir(logs.logDirectoryPath());
        const QStringList names = dir.entryList(QDir::Files);
        QCOMPARE(names.size(), 2);
        QVERIFY(names.contains(QStringLiteral("workspace.log")));
        QVERIFY(names.contains(QStringLiteral("workspace.log.1")));
        const QString rotated = readDiagnosticsLog(logs.rotatedFilePath());
        QVERIFY(rotated.startsWith('B'));
        QVERIFY(!rotated.startsWith('A'));
    }

    void diagnosticsMemorySnapshotMatchesShapeAndFixtureValues()
    {
        QTemporaryDir root;
        QTemporaryDir proc;
        QVERIFY(root.isValid());
        QVERIFY(proc.isValid());
        QVERIFY(QDir(proc.path()).mkpath(QStringLiteral("self")));
        {
            QFile status(proc.path() + QStringLiteral("/self/status"));
            QVERIFY(status.open(QIODevice::WriteOnly));
            status.write("Name:\tworkspace-native\nPid:\t4242\nVmSize:\t   20480 kB\n"
                         "VmHWM:\t   9216 kB\nVmRSS:\t   8192 kB\nThreads:\t7\n");
        }
        {
            QFile meminfo(proc.path() + QStringLiteral("/meminfo"));
            QVERIFY(meminfo.open(QIODevice::WriteOnly));
            meminfo.write("MemTotal:        8388608 kB\nMemFree:          524288 kB\n"
                          "MemAvailable:    4194304 kB\nSwapTotal:       1048576 kB\n");
        }
        NativeLogging logs(root.path());
        logs.setProcRoot(proc.path());
        const QVariantMap snapshot = logs.memorySnapshot();
        const QRegularExpression iso(QStringLiteral(
            "^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z$"));
        QVERIFY(iso.match(snapshot.value("collectedAt").toString()).hasMatch());
        const QVariantList processes = snapshot.value("processes").toList();
        QCOMPARE(processes.size(), 1);
        const QVariantMap self = processes.first().toMap();
        QVERIFY(self.value("pid").toInt() > 0);
        QCOMPARE(self.value("type").toString(), QStringLiteral("Main"));
#ifdef Q_OS_LINUX
        // Procfs-backed values exist only on Linux (the provider reads /proc
        // solely under Q_OS_LINUX; other platforms return zero values).
        const QVariantMap main = snapshot.value("main").toMap();
        QCOMPARE(main.value("rss").toLongLong(), qint64(8192) * 1024);
        QCOMPARE(main.value("size").toLongLong(), qint64(20480) * 1024);
        const QVariantMap system = snapshot.value("system").toMap();
        QCOMPARE(system.value("total").toLongLong(), qint64(8388608));
        QCOMPARE(system.value("free").toLongLong(), qint64(524288));
        QCOMPARE(system.value("available").toLongLong(), qint64(4194304));
        const QVariantMap memory = self.value("memory").toMap();
        QCOMPARE(memory.value("workingSetSize").toLongLong(), qint64(8192));
        QCOMPARE(memory.value("peakWorkingSetSize").toLongLong(), qint64(9216));
        QCOMPARE(snapshot.value("totalWorkingSetKb").toLongLong(), qint64(8192));
#endif
        // Missing proc sources degrade to the same key set with zero values.
        NativeLogging degraded(root.path());
        degraded.setProcRoot(root.path() + QStringLiteral("/missing-proc"));
        const QVariantMap flat = degraded.memorySnapshot();
        QVERIFY(iso.match(flat.value("collectedAt").toString()).hasMatch());
        QCOMPARE(flat.value("main").toMap().value("rss").toLongLong(), qint64(0));
        QCOMPARE(flat.value("system").toMap().value("total").toLongLong(), qint64(0));
        QCOMPARE(flat.value("processes").toList().size(), 1);
        QCOMPARE(flat.value("totalWorkingSetKb").toLongLong(), qint64(0));
    }

    void diagnosticsMemorySnapshotRealProcSanity()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        const QVariantMap snapshot = logs.memorySnapshot();
        // Key-set stability is platform-independent...
        QVERIFY(snapshot.contains(QStringLiteral("collectedAt")));
        QVERIFY(snapshot.contains(QStringLiteral("main")));
        QVERIFY(snapshot.contains(QStringLiteral("system")));
        QVERIFY(snapshot.contains(QStringLiteral("processes")));
        QVERIFY(snapshot.contains(QStringLiteral("totalWorkingSetKb")));
        const QVariantList processes = snapshot.value("processes").toList();
        QCOMPARE(processes.size(), 1);
        const QVariantMap self = processes.first().toMap();
        QCOMPARE(self.value("pid").toInt(), QCoreApplication::applicationPid());
        QCOMPARE(self.value("type").toString(), QStringLiteral("Main"));
#ifdef Q_OS_LINUX
        // Positive procfs values exist only on Linux; the provider reads
        // /proc solely under Q_OS_LINUX and other platforms return the same
        // keys with zero values (see docs/NATIVE_DESKTOP_DIAGNOSTICS.md and
        // the Q_OS_LINUX source gate guard case).
        QVERIFY(snapshot.value("main").toMap().value("rss").toLongLong() > 0);
        const QVariantMap system = snapshot.value("system").toMap();
        QVERIFY(system.value("total").toLongLong() > 0);
        QVERIFY(system.value("available").toLongLong() > 0);
        QVERIFY(system.value("available").toLongLong() <= system.value("total").toLongLong());
        QVERIFY(self.value("memory").toMap().value("workingSetSize").toLongLong() > 0);
        QVERIFY(snapshot.value("totalWorkingSetKb").toLongLong() > 0);
#endif
    }

    // ------------------------------------------------------------------
    // Issue #52: the memory panel provider (docs/NATIVE_DESKTOP_DIAGNOSTICS_
    // PAGE.md, "Memory panel" section). The dataset mirrors the Electron
    // diagnostics:getMemorySnapshot data the reference cards read
    // (workspace_ui diagnostics-memory.lib.ts:15-40 feeding
    // logs-page-memory.ui.tsx:96-197): per-process working set, peak and CPU
    // plus system total/free/available, with capability flags.
    // ------------------------------------------------------------------

    static bool writeMemoryProcFile(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(contents) == contents.size();
    }

    static bool seedMemoryProcFixture(const QString &root, qint64 rssKb, qint64 peakKb,
                                      qint64 totalKb = 8388608, qint64 freeKb = 524288,
                                      qint64 availableKb = 4194304)
    {
        if (!QDir(root).mkpath(QStringLiteral("self")))
            return false;
        return writeMemoryProcFile(root + QStringLiteral("/self/status"),
                QStringLiteral("Name:\tworkspace-native\nPid:\t4242\nVmSize:\t   20480 kB\n"
                               "VmHWM:\t   %1 kB\nVmRSS:\t   %2 kB\nThreads:\t7\n")
                    .arg(peakKb).arg(rssKb).toUtf8())
            && writeMemoryProcFile(root + QStringLiteral("/meminfo"),
                QStringLiteral("MemTotal:        %1 kB\nMemFree:          %2 kB\n"
                               "MemAvailable:    %3 kB\nSwapTotal:       1048576 kB\n")
                    .arg(totalKb).arg(freeKb).arg(availableKb).toUtf8());
    }

    void diagnosticsMemoryProviderParsesProcFixtures()
    {
        QTemporaryDir proc;
        QVERIFY(proc.isValid());
        QVERIFY(seedMemoryProcFixture(proc.path(), 8192, 9216));
        workspace::diagnostics::NativeMemoryDiagnostics provider;
        provider.setProcRoot(proc.path());
        const QVariantMap snapshot = provider.snapshot();
        const QRegularExpression iso(QStringLiteral(
            "^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z$"));
        QVERIFY(iso.match(snapshot.value(QStringLiteral("collectedAt")).toString()).hasMatch());
        QCOMPARE(snapshot.value(QStringLiteral("runtime")).toString(), QStringLiteral("native"));
        const QVariantMap app = snapshot.value(QStringLiteral("app")).toMap();
        const QVariantMap system = snapshot.value(QStringLiteral("system")).toMap();
        const QVariantMap capabilities = snapshot.value(QStringLiteral("capabilities")).toMap();
#ifdef Q_OS_LINUX
        QCOMPARE(app.value(QStringLiteral("workingSetKb")).toLongLong(), qint64(8192));
        QCOMPARE(app.value(QStringLiteral("peakWorkingSetKb")).toLongLong(), qint64(9216));
        QCOMPARE(app.value(QStringLiteral("totalWorkingSetKb")).toLongLong(), qint64(8192));
        QCOMPARE(system.value(QStringLiteral("totalKb")).toLongLong(), qint64(8388608));
        QCOMPARE(system.value(QStringLiteral("freeKb")).toLongLong(), qint64(524288));
        QCOMPARE(system.value(QStringLiteral("availableKb")).toLongLong(), qint64(4194304));
        // The system-availability rule the panel gates on: MemTotal > MemAvailable > 0.
        QVERIFY(system.value(QStringLiteral("totalKb")).toLongLong()
                > system.value(QStringLiteral("availableKb")).toLongLong());
        QVERIFY(system.value(QStringLiteral("availableKb")).toLongLong() > 0);
        QVERIFY(capabilities.value(QStringLiteral("processMetricsAvailable")).toBool());
        QVERIFY(capabilities.value(QStringLiteral("systemMemoryAvailable")).toBool());
        // Declared deviation: the single-process dataset renders exactly one
        // Main row with the reference five columns (logs-page-memory.ui.tsx:160-197).
        const QVariantList processes = snapshot.value(QStringLiteral("processes")).toList();
        QCOMPARE(processes.size(), 1);
        const QVariantMap self = processes.first().toMap();
        QCOMPARE(self.value(QStringLiteral("type")).toString(), QStringLiteral("Main"));
        QCOMPARE(self.value(QStringLiteral("pid")).toInt(), QCoreApplication::applicationPid());
        QCOMPARE(self.value(QStringLiteral("workingSetKb")).toLongLong(), qint64(8192));
        QCOMPARE(self.value(QStringLiteral("peakWorkingSetKb")).toLongLong(), qint64(9216));
        // No stat/uptime fixture files: CPU stays null (the panel draws an em dash).
        QVERIFY(self.value(QStringLiteral("cpuPercent")).isNull());
#else
        // Non-Linux keeps the key set with zeros and both capability flags false,
        // the documented shape the panel renders as the unavailable state.
        QVERIFY(!capabilities.value(QStringLiteral("processMetricsAvailable")).toBool());
        QVERIFY(!capabilities.value(QStringLiteral("systemMemoryAvailable")).toBool());
        QVERIFY(snapshot.value(QStringLiteral("processes")).toList().isEmpty());
        QCOMPARE(app.value(QStringLiteral("workingSetKb")).toLongLong(), qint64(0));
#endif
    }

    void diagnosticsMemoryProviderPeakIsMonotonicAcrossPolls()
    {
        QTemporaryDir proc;
        QVERIFY(proc.isValid());
        QVERIFY(seedMemoryProcFixture(proc.path(), 8192, 8192));
        workspace::diagnostics::NativeMemoryDiagnostics provider;
        provider.setProcRoot(proc.path());
#ifdef Q_OS_LINUX
        QCOMPARE(provider.snapshot().value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("peakWorkingSetKb")).toLongLong(), qint64(8192));
        // The fixture peak regresses below the live RSS: the reported peak must
        // never shrink (max over polls, VmHWM plus current RSS).
        QVERIFY(seedMemoryProcFixture(proc.path(), 12288, 8192));
        QCOMPARE(provider.snapshot().value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("peakWorkingSetKb")).toLongLong(), qint64(12288));
        QVERIFY(seedMemoryProcFixture(proc.path(), 5120, 5120));
        QCOMPARE(provider.snapshot().value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("peakWorkingSetKb")).toLongLong(), qint64(12288));
        QCOMPARE(provider.snapshot().value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("workingSetKb")).toLongLong(), qint64(5120));
#else
        QVERIFY(provider.snapshot().value(QStringLiteral("app")).toMap()
                    .value(QStringLiteral("peakWorkingSetKb")).toLongLong() == 0);
#endif
    }

    void diagnosticsMemoryProviderMalformedInputFallsBackToUnavailable()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("malformed/self")));
        QVERIFY(writeMemoryProcFile(root.path() + QStringLiteral("/malformed/self/status"),
            QStringLiteral("this is not a proc status document\n").toUtf8()));
        workspace::diagnostics::NativeMemoryDiagnostics malformed;
        malformed.setProcRoot(root.path() + QStringLiteral("/malformed"));
        const QVariantMap broken = malformed.snapshot();
        QVERIFY(broken.contains(QStringLiteral("capabilities")));
        const QVariantMap capabilities = broken.value(QStringLiteral("capabilities")).toMap();
        QVERIFY(!capabilities.value(QStringLiteral("processMetricsAvailable")).toBool());
        QVERIFY(!capabilities.value(QStringLiteral("systemMemoryAvailable")).toBool());
        QVERIFY(broken.value(QStringLiteral("processes")).toList().isEmpty());
        QCOMPARE(broken.value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("workingSetKb")).toLongLong(), qint64(0));
        QCOMPARE(broken.value(QStringLiteral("system")).toMap()
                     .value(QStringLiteral("totalKb")).toLongLong(), qint64(0));
        // A completely missing proc root degrades the same way (the panel
        // renders settings.diagnosticsMemoryUnavailable instead of cards).
        workspace::diagnostics::NativeMemoryDiagnostics missing;
        missing.setProcRoot(root.path() + QStringLiteral("/absent-proc"));
        const QVariantMap absent = missing.snapshot();
        QVERIFY(!absent.value(QStringLiteral("capabilities"))
                     .toMap().value(QStringLiteral("processMetricsAvailable")).toBool());
        QVERIFY(absent.value(QStringLiteral("processes")).toList().isEmpty());
    }

    // exordy review PRRT_kwDOVmpjls65Qz-z: the system free-memory card must
    // not fabricate a zero when MemFree is missing or malformed. The provider
    // tracks the MemFree parse result as its own capability flag so the card
    // gates on "parsed successfully" instead of the 0 fallback value.
    void diagnosticsMemoryProviderMissingMemFreeGatesFreeMetric()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("no-free/self")));
        QVERIFY(writeMemoryProcFile(root.path() + QStringLiteral("/no-free/self/status"),
            QByteArrayLiteral("Name:\tworkspace-native\nPid:\t4242\n"
                              "VmHWM:\t   9216 kB\nVmRSS:\t   8192 kB\n")));
        // MemTotal/MemAvailable parse cleanly; the MemFree line is absent.
        QVERIFY(writeMemoryProcFile(root.path() + QStringLiteral("/no-free/meminfo"),
            QByteArrayLiteral("MemTotal:        8388608 kB\n"
                              "MemAvailable:    4194304 kB\n")));
        workspace::diagnostics::NativeMemoryDiagnostics missingFree;
        missingFree.setProcRoot(root.path() + QStringLiteral("/no-free"));
        const QVariantMap missingFreeCaps =
            missingFree.snapshot().value(QStringLiteral("capabilities")).toMap();
        // The flag is part of the capability contract on every platform.
        QVERIFY2(missingFreeCaps.contains(QStringLiteral("systemMemoryFreeAvailable")),
                 qPrintable(QStringLiteral("capabilities keys: %1")
                     .arg(missingFreeCaps.keys().join(QLatin1Char(',')))));
        QVERIFY(!missingFreeCaps.value(QStringLiteral("systemMemoryFreeAvailable")).toBool());
#ifdef Q_OS_LINUX
        // total+available stay healthy, so systemMemoryAvailable alone is true:
        // it is MemFree's own flag that must veto the free-memory card.
        QVERIFY(missingFreeCaps.value(QStringLiteral("systemMemoryAvailable")).toBool());
        // A malformed MemFree value degrades identically to the missing line.
        QVERIFY(writeMemoryProcFile(root.path() + QStringLiteral("/no-free/meminfo"),
            QByteArrayLiteral("MemTotal:        8388608 kB\n"
                              "MemFree:         not-a-number kB\n"
                              "MemAvailable:    4194304 kB\n")));
        const QVariantMap malformedCaps =
            missingFree.snapshot().value(QStringLiteral("capabilities")).toMap();
        QVERIFY(!malformedCaps.value(QStringLiteral("systemMemoryAvailable")).toBool()
                || !malformedCaps.value(QStringLiteral("systemMemoryFreeAvailable")).toBool());
        QVERIFY(!malformedCaps.value(QStringLiteral("systemMemoryFreeAvailable")).toBool());
        // A fixture that parses MemFree cleanly opens the gate.
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("healthy/self")));
        QVERIFY(seedMemoryProcFixture(root.path() + QStringLiteral("/healthy"), 8192, 9216));
        workspace::diagnostics::NativeMemoryDiagnostics healthy;
        healthy.setProcRoot(root.path() + QStringLiteral("/healthy"));
        QVERIFY(healthy.snapshot().value(QStringLiteral("capabilities")).toMap()
                    .value(QStringLiteral("systemMemoryFreeAvailable")).toBool());
#endif
    }

#ifdef Q_OS_LINUX
    void diagnosticsMemoryCpuUsesOnlyPanelPolls()
    {
        QTemporaryDir proc;
        QVERIFY(proc.isValid());
        QVERIFY(seedMemoryProcFixture(proc.path(), 8192, 9216));
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.setDiagnosticsMemoryProcRoot(proc.path());
        const long hz = sysconf(_SC_CLK_TCK);
        QVERIFY(hz > 0);
        const auto sample = [&](int uptime, long ticks) {
            return writeMemoryProcFile(proc.filePath("uptime"), QByteArray::number(uptime) + " 0\n")
                && writeMemoryProcFile(proc.filePath("self/stat"),
                    "42 (workspace native) S 1 42 42 42 42 42 0 0 0 0 " + QByteArray::number(ticks)
                    + " 0 0 0 20 0 1 0 0\n");
        };
        const auto cpu = [&] {
            return controller.diagnosticsMemorySnapshot().value("processes").toList().first()
                .toMap().value("cpuPercent");
        };
        QVERIFY(sample(10000, hz));
        QVERIFY(cpu().isNull());
        QVERIFY(sample(10002, hz));
        controller.diagnosticsSnapshotJson(); // a report must not restart the interval
        QVERIFY(sample(10003, 4 * hz));
        QCOMPARE(cpu().toDouble(), 100.0);
        QVERIFY(sample(10006, 4 * hz));
        QCOMPARE(cpu(), QVariant(0.0));
        QVERIFY(sample(10009, 0));
        QVERIFY(cpu().isNull()); // counter reset
        QVERIFY(writeMemoryProcFile(proc.filePath("self/stat"), "broken\n"));
        QVERIFY(cpu().isNull());
        QVERIFY(sample(10012, 10 * hz));
        QVERIFY(cpu().isNull()); // restart after a missing sample
    }

#endif

    void diagnosticsSnapshotMemoryKeyUsesProviderWithoutLogging()
    {
        QTemporaryDir proc;
        QVERIFY(proc.isValid());
        QVERIFY(seedMemoryProcFixture(proc.path(), 8192, 9216));
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        // The memory key must be filled by the panel
        // provider (the reference cards never wait on the log writer).
        controller.setDiagnosticsMemoryProcRoot(proc.path());
        const QVariantMap memory =
            controller.diagnosticsSnapshot().value(QStringLiteral("memory")).toMap();
        QVERIFY2(!memory.isEmpty(), "diagnosticsSnapshot().memory must be the provider dataset");
        QCOMPARE(memory.value(QStringLiteral("runtime")).toString(), QStringLiteral("native"));
        QVERIFY(memory.contains(QStringLiteral("capabilities")));
        // The QML poll surface returns the same dataset.
        const QVariantMap invokable = controller.diagnosticsMemorySnapshot();
        QCOMPARE(invokable.value(QStringLiteral("runtime")).toString(), QStringLiteral("native"));
        QCOMPARE(invokable.value(QStringLiteral("capabilities")).toMap().keys().size(),
                 memory.value(QStringLiteral("capabilities")).toMap().keys().size());
#ifdef Q_OS_LINUX
        QCOMPARE(invokable.value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("workingSetKb")).toLongLong(), qint64(8192));
#endif
    }

    void diagnosticsMessageHandlerLineFormatAndFileWiring()
    {
        QCOMPARE(NativeLogging::formatMessageLine(QtDebugMsg, QStringLiteral("d")),
                 QStringLiteral("debug: d"));
        QCOMPARE(NativeLogging::formatMessageLine(QtInfoMsg, QStringLiteral("i")),
                 QStringLiteral("info: i"));
        QCOMPARE(NativeLogging::formatMessageLine(QtWarningMsg, QStringLiteral("w")),
                 QStringLiteral("warning: w"));
        QCOMPARE(NativeLogging::formatMessageLine(QtCriticalMsg, QStringLiteral("c")),
                 QStringLiteral("critical: c"));
        QCOMPARE(NativeLogging::formatMessageLine(QtFatalMsg, QStringLiteral("f")),
                 QStringLiteral("fatal: f"));
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        workspace::installNativeLogHandler(logs);
        QMessageLogger(nullptr, 0, nullptr, "qt.qml").warning("native diagnostics wiring probe");
        qInfo("native diagnostics info probe");
        workspace::removeNativeLogHandler();
        const QString text = readDiagnosticsLog(logs.filePath());
        QVERIFY2(text.contains(QStringLiteral("warning: QML diagnostic\n")),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("info: Qt diagnostic\n")),
                 qPrintable(text));
    }
    // ------------------------------------------------------------------
    // exordy fix-round-1 regression cases (PR #289 review findings).
    // ------------------------------------------------------------------

    // Finding 1 (thread 4575150367): a stderr write failure must not gate
    // persistence — the line still has to reach the diagnostics file.
    // Descriptor surgery on fd 2 is POSIX-only (round-2 finding 4575693897),
    // so the case compiles and runs on Unix platforms only.
#if defined(Q_OS_UNIX)
    void persistentQtDiagnosticsOmitPrivateMessageAndCategoryValues()
    {
        QTemporaryDir root;
        NativeLogging logs(root.path());
        QVERIFY(root.isValid());
        workspace::installNativeLogHandler(logs);
        const auto removeHandler = qScopeGuard([] { workspace::removeNativeLogHandler(); });
        QMessageLogger(nullptr, 0, nullptr, "qt.network.private-category").warning(
            "synthetic-body account@example.invalid /home/synthetic-user/file https://example.invalid/?private-value");
        qInfo("Default topic missing for existing direct stream before call");
        QVERIFY(QFile::resize(logs.filePath(), NativeLogging::MaxLogFileBytes));
        QMessageLogger(nullptr, 0, nullptr, "qt.qpa.private-category").warning(
            "{\"context\":{\"body\":\"private-body\"}}");
        const auto previous = readDiagnosticsLog(logs.rotatedFilePath());
        const auto current = readDiagnosticsLog(logs.filePath());
        QVERIFY(previous.contains(QStringLiteral("warning: Network diagnostic\n")));
        QVERIFY(previous.contains(QStringLiteral("info: Default topic missing for existing direct stream before call\n")));
        QVERIFY(current.contains(QStringLiteral("warning: Platform diagnostic\n")));
        for (const auto *value : {"synthetic", "example.invalid", "/home/", "private-"}) {
            QVERIFY(!previous.contains(QString::fromLatin1(value)));
            QVERIFY(!current.contains(QString::fromLatin1(value)));
        }
    }

    void diagnosticsMessageHandlerAppendsToFileWhenStderrFails()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NativeLogging logs(root.path());
        QVERIFY(!logs.getFilePath().isEmpty());
        workspace::installNativeLogHandler(logs);
        // Repoint fd 2 at a read-only descriptor so every stderr write fails
        // with EBADF (stderr is unbuffered, so fwrite reports the failure).
        const int savedStderr = ::dup(STDERR_FILENO);
        QVERIFY(savedStderr >= 0);
        const int readOnly = ::open(logs.logDirectoryPath().toLocal8Bit().constData(), O_RDONLY);
        QVERIFY(readOnly >= 0);
        fflush(stderr);
        QVERIFY(::dup2(readOnly, STDERR_FILENO) == STDERR_FILENO);
        ::close(readOnly);
        qWarning("stderr unavailable probe");
        workspace::removeNativeLogHandler();
        QVERIFY(::dup2(savedStderr, STDERR_FILENO) == STDERR_FILENO);
        ::close(savedStderr);
        fflush(stderr);
        clearerr(stderr);
        const QString text = readDiagnosticsLog(logs.filePath());
        QVERIFY2(text.contains(QStringLiteral("warning: Qt diagnostic\n")),
                 qPrintable(text));
    }
#endif // Q_OS_UNIX

    // Finding 2 (thread 4575150368): the procfs sources are Linux-only. This
    // guard keeps that gate on the provider: without it the ungated form
    // reaches for /proc on platforms where the values are legitimately zero.
    void diagnosticsMemorySnapshotProcfsSourcesGateWithQosLinux()
    {
        QFile source(QStringLiteral(WORKSPACE_NATIVE_DIAGNOSTICS_SOURCE));
        QVERIFY2(source.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QStringLiteral("cannot open %1")
                                .arg(QLatin1String(WORKSPACE_NATIVE_DIAGNOSTICS_SOURCE))));
        const QString text = QString::fromUtf8(source.readAll());
        const int start = text.indexOf(
            QStringLiteral("QVariantMap NativeLogging::memorySnapshot() const"));
        QVERIFY2(start >= 0, "memorySnapshot() definition not found in the diagnostics source");
        const int end = text.indexOf(QLatin1String("\n}"), start);
        QVERIFY(end > start);
        const QString body = text.mid(start, end - start);
        QVERIFY2(body.contains(QLatin1String("Q_OS_LINUX")),
                 "memorySnapshot() must read procfs only under Q_OS_LINUX so "
                 "non-Linux builds keep the documented zero-value shape with no "
                 "/proc access; the real-proc sanity asserts stay gated");
    }

    // ------------------------------------------------------------------
    // exordy fix-round-2 regression cases (PR #289 review findings).
    // ------------------------------------------------------------------

    // Finding 1 (thread 4575693897): the diagnostics block added POSIX-only
    // headers and raw descriptor calls, which broke BUILD_TESTING on Windows.
    // This source guard keeps that contract on the test file itself (it
    // mirrors the Q_OS_LINUX source-gate case above and must fail on every
    // platform): every POSIX header include and every line-leading raw POSIX
    // call in this translation unit must sit inside a Q_OS_UNIX/Q_OS_LINUX/
    // Q_OS_POSIX preprocessor guard. The platform-neutral code uses the
    // portable wrappers in msvc-compat.h instead.
    void diagnosticsTestPosixApisGuardedWithQosUnix()
    {
        QFile source(QStringLiteral(__FILE__));
        QVERIFY2(source.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QStringLiteral("cannot open %1")
                                .arg(QDir::toNativeSeparators(QStringLiteral(__FILE__)))));
        const QStringList lines =
            QString::fromUtf8(source.readAll()).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        static const QVector<QString> posixIncludes {
            QStringLiteral("<fcntl.h>"), QStringLiteral("<poll.h>"),
            QStringLiteral("<sys/stat.h>"), QStringLiteral("<unistd.h>")
        };
        static const QRegularExpression ifRe(
            QStringLiteral("^#[ \\t]*(if|ifdef|ifndef)\\b"));
        static const QRegularExpression elseRe(
            QStringLiteral("^#[ \\t]*(else|elif)\\b"));
        static const QRegularExpression endRe(
            QStringLiteral("^#[ \\t]*endif\\b"));
        static const QRegularExpression callRe(QStringLiteral(
            "^(?:::)?(?:pipe|dup2|dup|open|close|read|write|poll|fcntl|fflush|clearerr|fsync"
            "|access|unlink|getpid|isatty)\\s*\\("));
        QStringList unguardedIncludes;
        QStringList unguardedCalls;
        QVector<bool> guardStack; // one entry per open #if; true = Q_OS_UNIX-style guard
        for (int i = 0; i < lines.size(); ++i) {
            const QString raw = lines.at(i).trimmed();
            if (raw.startsWith(QLatin1Char('#'))) {
                bool directiveHandled = true;
                if (ifRe.match(raw).hasMatch()) {
                    guardStack.append(raw.contains(QLatin1String("Q_OS_UNIX"))
                                      || raw.contains(QLatin1String("Q_OS_LINUX"))
                                      || raw.contains(QLatin1String("Q_OS_POSIX")));
                } else if (elseRe.match(raw).hasMatch()) {
                    if (!guardStack.isEmpty())
                        guardStack.last() = false; // #else/#elif branch is unguarded
                } else if (endRe.match(raw).hasMatch()) {
                    if (!guardStack.isEmpty())
                        guardStack.removeLast();
                } else {
                    directiveHandled = false; // e.g. #include lines still need auditing
                }
                if (directiveHandled)
                    continue;
            }
            bool guarded = !guardStack.isEmpty();
            for (bool open : std::as_const(guardStack))
                guarded = guarded && open;
            if (guarded)
                continue;
            if (raw.startsWith(QLatin1String("#include"))) {
                for (const QString &header : posixIncludes) {
                    if (raw.contains(header))
                        unguardedIncludes << QStringLiteral("line %1: %2").arg(i + 1).arg(raw);
                }
            } else if (callRe.match(raw).hasMatch()) {
                unguardedCalls << QStringLiteral("line %1: %2").arg(i + 1).arg(raw.left(72));
            }
        }
        QVERIFY2(unguardedIncludes.isEmpty(),
                 qPrintable(QStringLiteral("POSIX-only includes must sit inside a "
                                            "Q_OS_UNIX/Q_OS_LINUX guard:\n%1")
                                .arg(unguardedIncludes.join(QLatin1Char('\n')))));
        QVERIFY2(unguardedCalls.isEmpty(),
                 qPrintable(QStringLiteral("raw POSIX descriptor calls must sit inside a "
                                            "Q_OS_UNIX/Q_OS_LINUX guard; platform-neutral code "
                                            "must use the msvc-compat.h wrappers instead:\n%1")
                                .arg(unguardedCalls.join(QLatin1Char('\n')))));
    }

    // Finding 3 (thread 4575150370): removing the handler must drain
    // in-flight handler calls on other threads before returning.
    // Round-2 shape (thread 4575693899): the case no longer assumes a 64 KiB
    // pipe capacity. The reader is started before any write can block; the
    // filler fills the pipe with O_NONBLOCK writes probed with 1-byte writes
    // until EAGAIN (works at 4 KiB or 64 KiB); and on Linux the pipe is
    // pinned to its 4 KiB minimum so the small-capacity path is always
    // exercised. Once the pipe is full, stderr returns to blocking mode and
    // the worker's 32 KB qWarning parks mid stderr write — an in-flight
    // handler call for an unbounded time. The reader only starts draining
    // 100 ms after the removal began: a synchronized removeNativeLogHandler()
    // cannot return before that release lands (the in-flight call finishes
    // and its line reaches the file), while the unsynchronized version
    // returns immediately and would let this scope destroy the stack-owned
    // sink under the still-parked handler. Descriptor work is POSIX-only
    // (round-2 finding 4575693897), so the case compiles on Unix only.
#if defined(Q_OS_UNIX)
    void diagnosticsMessageHandlerRemovalDrainsInFlightWrites()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        std::atomic<bool> workerReturned{false};
        std::atomic<bool> drainAtRemove{false};
        std::atomic<bool> removalRequested{false};
        // Exit-path contract (thread 4575792864): every exit, success or a
        // failed assertion, must release the reader BEFORE joining it (and
        // before joining the worker), or an early failure joins a releaser
        // still waiting for removalRequested and the case hangs forever.
        std::atomic<bool> cleanupRequested{false};
        qint64 releaserDrained = 0;

        {
            NativeLogging logs(root.path());
            QVERIFY(!logs.getFilePath().isEmpty());

            int consolePipe[2] = { -1, -1 };
            QVERIFY(::pipe(consolePipe) == 0);
#ifdef Q_OS_LINUX
            // Pin the pipe to the minimum size (4096 bytes, the value that
            // made the pre-fix 64 KiB assumption hang) so the capacity
            // independence below is asserted, not accidental.
            ::fcntl(consolePipe[1], F_SETPIPE_SZ, 4096);
#endif
            const int savedStderr = ::dup(STDERR_FILENO);
            QVERIFY(savedStderr >= 0);
            auto stderrGuard = qScopeGuard([&] {
                nativeTestFlushStderr();
                ::dup2(savedStderr, STDERR_FILENO);
                ::close(savedStderr);
                ::close(consolePipe[0]);
                ::close(consolePipe[1]);
                nativeTestClearErr();
            });
            nativeTestFlushStderr();
            QVERIFY(::dup2(consolePipe[1], STDERR_FILENO) == STDERR_FILENO);
            // Fill the pipe in nonblocking mode: no filler write can park the
            // main thread regardless of pipe capacity.
            const int blockingFlags = ::fcntl(consolePipe[1], F_GETFL, 0);
            QVERIFY(blockingFlags >= 0);
            QVERIFY(::fcntl(consolePipe[1], F_SETFL, blockingFlags | O_NONBLOCK) == 0);

            workspace::installNativeLogHandler(logs);

            // The reader exists before anything can block. It holds the pipe
            // full until 100 ms after the removal began, then drains until
            // the parked worker line has fully left the pipe.
            std::thread releaser([&]() {
                while (!removalRequested.load() && !cleanupRequested.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                // Release only 100 ms after the removal began: that is the
                // window the synchronized removal must be seen waiting in.
                // On the cleanup exit path (an assertion failed before the
                // removal was requested) drain immediately: a parked worker
                // must be released so its join below cannot hang either.
                QElapsedTimer holdClock;
                holdClock.start();
                while (removalRequested.load() && holdClock.elapsed() < 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                char buf[16384];
                QElapsedTimer clock;
                clock.start();
                while (clock.elapsed() < 5000) {
                    const int n = nativeTestPollRead(consolePipe[0], buf, sizeof buf, 50);
                    if (n < 0)
                        break;
                    releaserDrained += n;
                    if (n == 0 && workerReturned.load())
                        break;
                }
            });
            auto releaserGuard = qScopeGuard([&] {
                cleanupRequested.store(true);
                if (releaser.joinable())
                    releaser.join();
            });

            // Nonblocking filler lines occupy the pipe up to exactly its
            // capacity; each iteration ends with a 1-byte probe that reports
            // EAGAIN once the pipe is full. No capacity assumption anywhere.
            const QByteArray probe(1, '\n');
            for (int i = 0; i < 512; ++i) {
                qWarning().noquote() << QStringLiteral("filler ")
                                        + QString(8000, QLatin1Char('F'));
                nativeTestFlushStderr();
                if (::write(STDERR_FILENO, probe.constData(), probe.size()) != 1)
                    break; // EAGAIN: the pipe is full at this capacity
            }
            // The filler left the stream in a would-block state; restore
            // blocking writes and clear the error flag before the worker
            // parks inside its stderr write.
            QVERIFY(::fcntl(consolePipe[1], F_SETFL, blockingFlags) == 0);
            nativeTestClearErr();

            std::thread worker([&]() {
                QMessageLogger(nullptr, 0, nullptr, "qt.network").warning().noquote() << QStringLiteral("worker in-flight line ")
                                        + QString(32000, QLatin1Char('W'));
                workerReturned.store(true);
            });
            auto workerGuard = qScopeGuard([&] {
                // Runs before releaserGuard (reverse declaration order):
                // release + drain first so a parked worker can finish, then
                // join both threads.
                cleanupRequested.store(true);
                if (releaser.joinable())
                    releaser.join();
                if (worker.joinable())
                    worker.join();
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            QVERIFY(!workerReturned.load()); // parked mid stderr write

            removalRequested.store(true);
            QElapsedTimer removalClock;
            removalClock.start();
            workspace::removeNativeLogHandler();
            const qint64 removalWaitMs = removalClock.elapsed();
            // Draining means the removal waited for the parked in-flight
            // call; the only release happens 100 ms after the removal starts.
            drainAtRemove.store(removalWaitMs >= 50);
            qInfo("post-removal diagnostics line"); // default handler only
        } // threads joined and stderr restored by the scope guards

        QVERIFY2(drainAtRemove.load(),
                 "removeNativeLogHandler() must wait for in-flight handler "
                 "calls to drain before returning so no thread consults a "
                 "sink that removal has already cleared");
        QVERIFY(workerReturned.load());
        // The worker's whole 32 KB line (plus the filler that filled the
        // pipe) must have flowed through the reader: capacity-independent.
        QVERIFY(releaserDrained >= 32000);
        const QString text = readDiagnosticsLog(
            root.path() + QStringLiteral("/workspace_logs/workspace.log"));
        QVERIFY2(text.contains(QStringLiteral("warning: Qt diagnostic\n")), qPrintable(text.left(120)));
        QVERIFY2(text.contains(QStringLiteral("warning: Network diagnostic\n")),
                 "drained in-flight lines must reach the file");
        QVERIFY(!text.contains(QStringLiteral("info: Qt diagnostic\n")));
    }
#endif // Q_OS_UNIX

    // ---- Issue #50: diagnostics dashboard (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md)
    void diagnosticsOverallStatusTable_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::addColumn<bool>("online");
        QTest::addColumn<int>("recentErrors");
        QTest::addColumn<QString>("status");
        // Reference rule (diagnostics-collect.lib.ts:153-188): offline wins
        // over everything; any non-clean phase or a recent log error degrades;
        // only a clean online phase is healthy.
        QTest::newRow("offline-phase") << QStringLiteral("offline") << true << 0 << QStringLiteral("offline");
        QTest::newRow("transport-down") << QStringLiteral("online") << false << 0 << QStringLiteral("offline");
        QTest::newRow("healthy") << QStringLiteral("online") << true << 0 << QStringLiteral("healthy");
        QTest::newRow("reconnecting") << QStringLiteral("reconnecting") << true << 0 << QStringLiteral("degraded");
        QTest::newRow("rate-limited") << QStringLiteral("rate_limited") << true << 0 << QStringLiteral("degraded");
        QTest::newRow("degraded-phase") << QStringLiteral("degraded") << true << 0 << QStringLiteral("degraded");
        QTest::newRow("connecting") << QStringLiteral("connecting") << true << 0 << QStringLiteral("degraded");
        QTest::newRow("blocked") << QStringLiteral("blocked") << true << 0 << QStringLiteral("degraded");
        QTest::newRow("recent-errors-force-degraded") << QStringLiteral("online") << true << 1 << QStringLiteral("degraded");
        QTest::newRow("offline-wins-over-errors") << QStringLiteral("offline") << true << 3 << QStringLiteral("offline");
    }
    void diagnosticsOverallStatusTable()
    {
        QFETCH(QString, phase); QFETCH(bool, online);
        QFETCH(int, recentErrors); QFETCH(QString, status);
        QCOMPARE(workspace::diagnostics::resolveOverallStatus(phase, online, recentErrors), status);
    }

    void diagnosticsSessionLogSummaryBucketsLevelsAndRecentWindow()
    {
        using namespace workspace::diagnostics;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        SessionLogBuffer buffer;
        buffer.append("info", "app", "hello", {}, now);
        buffer.append("debug", "app", "d1", {}, now);
        buffer.append("warn", "app", "w1", {}, now);
        buffer.append("error", "api", "outside window", {}, now - 16 * 60 * 1000);
        for (int index = 10; index >= 1; --index)
            buffer.append("error", "api", QStringLiteral("recent %1").arg(index), {},
                now - qint64(index) * 60 * 1000);
        const auto summary = buffer.summary(now);
        QCOMPARE(summary.infoCount, 1);
        QCOMPARE(summary.debugCount, 1);
        QCOMPARE(summary.warnCount, 1);
        QCOMPARE(summary.errorCount, 11); // 10 recent + 1 old
        QCOMPARE(summary.recentErrorCount, 10);
        QCOMPARE(summary.recentErrors.size(), RecentErrorLimit);
        QCOMPARE(summary.recentErrors.first().message, QStringLiteral("recent 1")); // newest first
        QCOMPARE(summary.recentErrors.last().message, QStringLiteral("recent 8")); // oldest two trimmed by the cap
        QCOMPARE(summary.recentErrors.first().level, QStringLiteral("error"));
    }

    // Review 4611502317 on #404: the parser stripped the 'Z' suffix before
    // parsing, so Qt read the stamp in the machine-local zone. Recent errors
    // could be missed (east of UTC shifts stamps into the past) or stay
    // recent for hours (west of UTC shifts them into the future). Pin a
    // non-UTC zone so the regression fails for the right reason on any host.
    void diagnosticsSessionLogRecentWindowParsesStampsInUtcAcrossLocalZones()
    {
        using namespace workspace::diagnostics;
        const QByteArray previousZone = qgetenv("TZ");
        const auto restoreZone = qScopeGuard([previousZone] { qputenv("TZ", previousZone); tzset(); });
        Q_UNUSED(restoreZone);
        const qint64 base = QDateTime::fromString(
            QStringLiteral("2026-01-02T12:00:00.000Z"), Qt::ISODateWithMs).toMSecsSinceEpoch();
        const qint64 now = base + 30 * 1000; // "recent-a" is 30 s old, "outside-b" is 16 min old
        SessionLogBuffer buffer;
        buffer.append("error", "api", "outside-b", {}, base - 16 * 60 * 1000);
        buffer.append("error", "api", "recent-a", {}, base);
        for (const auto *zone : { "Etc/GMT+5", "Etc/GMT-5" }) {
            qputenv("TZ", zone);
            tzset();
            // Sanity: the pinned zone must really reach QDateTime, otherwise
            // a UTC-only host would hide the regression instead of exposing it.
            const auto shift = QDateTime::fromString(
                QStringLiteral("2026-01-02T12:00:00.000"), Qt::ISODateWithMs).toMSecsSinceEpoch() - base;
            QVERIFY2(shift == qint64(5) * 3600 * 1000 || shift == -qint64(5) * 3600 * 1000,
                QByteArray(zone) + ": the pinned zone did not take effect for Qt");
            const auto summary = buffer.summary(now);
            QCOMPARE(summary.errorCount, 2);
            QCOMPARE(summary.recentErrorCount, 1); // only "recent-a" sits in the 15 min window
            QCOMPARE(summary.recentErrors.size(), 1);
            if (summary.recentErrors.size() == 1)
                QCOMPARE(summary.recentErrors.first().message, QStringLiteral("recent-a"));
        }
    }

    void diagnosticsLatencySummaryMatchesReferencePercentiles()
    {
        using namespace workspace::diagnostics;
        const auto empty = summarizeLatency({});
        QCOMPARE(empty.sampleCount, 0);
        QCOMPARE(empty.medianMs, qint64(-1)); // reference null
        QCOMPARE(empty.p95Ms, qint64(-1));
        const auto few = summarizeLatency({40, 10, 30, 20});
        QCOMPARE(few.sampleCount, 4);
        QCOMPARE(few.medianMs, qint64(30)); // sorted[floor(4*0.5)]
        QCOMPARE(few.p95Ms, qint64(40));    // sorted[min(3, floor(4*0.95))]
        QList<qint64> many;
        for (qint64 value = 1; value <= 20; ++value)
            many.append(value);
        const auto full = summarizeLatency(many);
        QCOMPARE(full.medianMs, qint64(11)); // sorted[10]
        QCOMPARE(full.p95Ms, qint64(20));     // sorted[19]
    }

    void diagnosticsSectionAndDeepLinksRequireAuthenticationAndOrigin()
    {
        Fixture f;
        // Offline the section never renders (the login overlay keeps chat).
        f.controller.showSection(QStringLiteral("diagnostics"));
        QVERIFY(f.controller.section() != QStringLiteral("diagnostics"));
        f.controller.openMessageLink(QStringLiteral("ew://open/settings/logs"));
        QVERIFY(f.controller.section() != QStringLiteral("diagnostics"));
        QVERIFY(f.start());
        // The reference web route (right-panel-user-menu.ui.tsx:182 navigates
        // to /settings/logs) and the bare /logs variant both open the page.
        f.controller.openMessageLink(QStringLiteral("ew://open/settings/logs"));
        QCOMPARE(f.controller.section(), QStringLiteral("diagnostics"));
        f.controller.showSection(QStringLiteral("chat"));
        f.controller.openMessageLink(QStringLiteral("ew://open/logs"));
        QCOMPARE(f.controller.section(), QStringLiteral("diagnostics"));
        // The org-prefixed variant carries the shared origin guard: a foreign
        // org is ignored, the matching org opens the page.
        f.controller.showSection(QStringLiteral("chat"));
        f.controller.openMessageLink(QStringLiteral("ew://open/org/foreign-org/logs"));
        QCOMPARE(f.controller.section(), QStringLiteral("chat"));
        f.controller.openMessageLink(QStringLiteral("ew://open/org/%1/settings/logs")
            .arg(workspace::orgRouteIdFromOrigin(f.server.origin())));
        QCOMPARE(f.controller.section(), QStringLiteral("diagnostics"));
    }

    void diagnosticsSnapshotDatasetsReportAndPing()
    {
        Fixture f; QVERIFY(f.start());
        // Pin the memory provider to a fixed proc fixture so the two snapshot
        // reads below see identical process-state values (live /proc RSS moves
        // between calls; issue #52 memory panel dataset).
        QTemporaryDir memoryProc; QVERIFY(memoryProc.isValid());
        QVERIFY(seedMemoryProcFixture(memoryProc.path(), 8192, 9216));
        f.controller.setDiagnosticsMemoryProcRoot(memoryProc.path());
        const auto snapshot = f.controller.diagnosticsSnapshot();
        // The dataset shape the page binds against (reference snapshot keys).
        for (const auto *key:{"overallStatus", "collectedAt", "connection", "realtime",
                              "session", "workspaceSession", "runtime", "environment",
                              "notifications", "performance", "logs", "stores"})
            QVERIFY2(snapshot.contains(QLatin1String(key)), key);
        QCOMPARE(snapshot.value(QStringLiteral("overallStatus")).toString(),
                 f.controller.diagnosticsOverallStatus());
        QCOMPARE(snapshot.value(QStringLiteral("connection")).toMap()
                     .value(QStringLiteral("phase")).toString(), f.controller.connectionPhase());
        // The fixture login produced real HTTP round trips, so the latency
        // dataset carries live samples (exact percentile math is pinned by
        // diagnosticsLatencySummaryMatchesReferencePercentiles).
        const auto apiLatency = snapshot.value(QStringLiteral("performance")).toMap()
                                    .value(QStringLiteral("apiLatency")).toMap();
        QVERIFY(apiLatency.value(QStringLiteral("sampleCount")).toInt() >= 1);
        const auto medianMs = apiLatency.value(QStringLiteral("medianMs"));
        const auto p95Ms = apiLatency.value(QStringLiteral("p95Ms"));
        QVERIFY(!medianMs.isNull() && medianMs.toLongLong() >= 0);
        QVERIFY(!p95Ms.isNull() && p95Ms.toLongLong() >= medianMs.toLongLong());
        const auto slowest = apiLatency.value(QStringLiteral("slowest")).toList();
        QVERIFY(!slowest.isEmpty() && slowest.size() <= 3);
        for (qsizetype index = 1; index < slowest.size(); ++index)
            QVERIFY(slowest.at(index - 1).toMap().value(QStringLiteral("durationMs")).toLongLong()
                    >= slowest.at(index).toMap().value(QStringLiteral("durationMs")).toLongLong());
        QCOMPARE(snapshot.value(QStringLiteral("stores")).toMap()
                     .value(QStringLiteral("streamsCount")).toInt(), 1);
        QVERIFY(!snapshot.value(QStringLiteral("workspaceSession")).toMap()
                     .value(QStringLiteral("login")).toString().isEmpty());
        // Copy surfaces: the snapshot is the pretty two-space full dataset,
        // the connection report the reference subset (no stores card data).
        const auto json = f.controller.diagnosticsSnapshotJson();
        QVERIFY(json.contains(QLatin1String("\n  \"overallStatus\"")));
        QVERIFY(!QJsonDocument::fromJson(json.toUtf8()).isNull());
        // collectedAt and session.sessionRemainingMs advance with the wall
        // clock and are re-sampled on every call, so the export equality must
        // ignore them: comparing the two reads whole made this case flaky
        // (a one-millisecond tick between the reads failed the equality).
        const auto stripClockDriven = [](QJsonObject object) {
            object.remove(QStringLiteral("collectedAt"));
            auto session = object.value(QStringLiteral("session")).toObject();
            session.remove(QStringLiteral("sessionRemainingMs"));
            object.insert(QStringLiteral("session"), session);
            auto memory = object.value(QStringLiteral("memory")).toObject();
            memory.remove(QStringLiteral("collectedAt"));
            object.insert(QStringLiteral("memory"), memory);
            return object;
        };
        QCOMPARE(stripClockDriven(QJsonDocument::fromJson(json.toUtf8()).object()),
                 stripClockDriven(QJsonDocument::fromVariant(snapshot).object()));
        const auto report = QJsonDocument::fromJson(
            f.controller.diagnosticsConnectionReportJson().toUtf8()).object();
        for (const auto *key:{"collectedAt", "overallStatus", "connection", "realtime",
                              "workspaceSession", "recentErrors"})
            QVERIFY2(report.contains(QLatin1String(key)), key);
        QVERIFY(!report.contains(QStringLiteral("stores")));
        // Ping: one round trip, one signal; a second call while in flight is
        // ignored by the reference single-probe guard.
        QSignalSpy finished(&f.controller, &NativeController::pingFinished);
        f.controller.pingServer();
        f.controller.pingServer();
        QVERIFY(finished.wait(5000));
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.first().at(0).toBool());
        QVERIFY(finished.first().at(1).toInt() >= 0);
    }

    void diagnosticsPingRequiresAuthentication()
    {
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QSignalSpy finished(&controller, &NativeController::pingFinished);
        controller.pingServer();
        QTest::qWait(250);
        QCOMPARE(finished.count(), 0);
    }

    // Review 4611502344 on #404: a ping in flight when the API generation
    // changes (token loss, account or server switch) never sees its transport
    // callback — the request wrapper drops retired callbacks — so the
    // in-flight flag and the page's loading state used to stay stuck forever.
    // The generation bump must settle the ping as a failed probe and free the
    // guard so later pings can dispatch again.
    void diagnosticsPingSettlesWhenGenerationBumpRetiresTheCallback()
    {
        Fixture f;
        int pingGets = 0;
        std::function<void()> release;
        f.streams = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).queryItemValue("page_limit") != QLatin1String("1"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}}}};
            ++pingGets;
            Response response{QJsonArray{}};
            response.defer = [&](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        QVERIFY(f.start());
        QSignalSpy finished(&f.controller, &NativeController::pingFinished);
        f.controller.pingServer();
        QVERIFY(QTest::qWaitFor([&] { return pingGets == 1; }, 2000));
        QCOMPARE(finished.count(), 0); // the answer is held by the gate
        f.api.clearSession(); // token/session change: the bump retires the callback
        QCOMPARE(finished.count(), 1); // the bump settles the ping as a failed probe
        QCOMPARE(finished.first().at(0).toBool(), false);
        QCOMPARE(finished.first().at(1).toInt(), -1);
        if (release)
            release(); // the retired answer must not settle the ping a second time
        QTest::qWait(200);
        QCOMPARE(finished.count(), 1);
        // The single-probe guard is freed: a fresh ping dispatches under the
        // new generation and settles normally (with no token the transport
        // would reject locally, so re-arm a credential first).
        f.api.setAccessToken("rotated-token");
        f.controller.pingServer();
        QVERIFY(QTest::qWaitFor([&] { return pingGets == 2; }, 2000));
        QVERIFY(release);
        release();
        QVERIFY(finished.wait(2000));
        QCOMPARE(finished.count(), 2);
        QCOMPARE(finished.at(1).at(0).toBool(), true);
    }

    // ---- Issue #51: session log viewer modal (docs: modal section) --------

    // The reference logger ring buffer (shared/lib/logger.ts): append notifies
    // subscribers, newest-first reads, oldest-first eviction at 500, and clear
    // notifies once.
    void sessionLogBufferAppendSubscribeEvictClear()
    {
        using namespace workspace::diagnostics;
        SessionLogBuffer buffer;
        QSignalSpy changed(&buffer, &SessionLogBuffer::entriesChanged);
        buffer.append(QStringLiteral("info"), QStringLiteral("app"), QStringLiteral("first"));
        buffer.append(QStringLiteral("warn"), QStringLiteral("api"), QStringLiteral("second"),
            QVariantMap{{QStringLiteral("status"), 429}});
        QCOMPARE(changed.count(), 2);
        const auto entries = buffer.entries();
        QCOMPARE(entries.size(), qsizetype(2));
        QCOMPARE(entries.at(0).message, QStringLiteral("second")); // newest first
        QCOMPARE(entries.at(0).level, QStringLiteral("warn"));
        QVERIFY(entries.at(0).hasData);
        QVERIFY(!entries.at(1).hasData);
        QCOMPARE(entries.at(1).message, QStringLiteral("first"));
        // Timestamp is ISO-8601 UTC with ms (reference new Date().toISOString()).
        const auto stamp = QDateTime::fromString(entries.at(0).timestamp, Qt::ISODateWithMs);
        QVERIFY(stamp.isValid());
        QCOMPARE(stamp.timeSpec(), Qt::UTC);
        buffer.clear();
        QCOMPARE(changed.count(), 3);
        QVERIFY(buffer.entries().isEmpty());
        for (int index = 0; index < SessionLogBuffer::MaxEntries + 5; ++index)
            buffer.append(QStringLiteral("info"), QStringLiteral("app"),
                QStringLiteral("line-%1").arg(index));
        const auto capped = buffer.entries();
        QCOMPARE(capped.size(), qsizetype(SessionLogBuffer::MaxEntries));
        QCOMPARE(capped.first().message,
            QStringLiteral("line-%1").arg(SessionLogBuffer::MaxEntries + 4));
        QCOMPARE(capped.last().message, QStringLiteral("line-5")); // oldest 5 evicted
    }

    // logs-source-filter.lib.ts:3-32 bucket rules.
    void sessionLogSourceBucketsMatchReferenceRules()
    {
        using namespace workspace::diagnostics;
        QCOMPARE(logSourceBucket(QStringLiteral("api")), QStringLiteral("api"));
        QCOMPARE(logSourceBucket(QStringLiteral("console")), QStringLiteral("console"));
        QCOMPARE(logSourceBucket(QStringLiteral("trace:chat-list")), QStringLiteral("trace"));
        QCOMPARE(logSourceBucket(QStringLiteral("action")), QStringLiteral("actions"));
        QCOMPARE(logSourceBucket(QStringLiteral("auth")), QStringLiteral("actions"));
        QCOMPARE(logSourceBucket(QStringLiteral("realtime")), QStringLiteral("actions"));
        QCOMPARE(logSourceBucket(QStringLiteral("action:switch")), QStringLiteral("actions"));
        QCOMPARE(logSourceBucket(QStringLiteral("store:messages")), QStringLiteral("app"));
        QCOMPARE(logSourceBucket(QStringLiteral("app")), QStringLiteral("app"));
        QCOMPARE(logSourceBucket(QStringLiteral("connection-health")), QStringLiteral("app"));
        QVERIFY(matchesLogSourceFilter(QStringLiteral("anything"), QStringLiteral("all")));
        QVERIFY(matchesLogSourceFilter(QStringLiteral("trace:x"), QStringLiteral("trace")));
        QVERIFY(!matchesLogSourceFilter(QStringLiteral("trace:x"), QStringLiteral("actions")));
        QVERIFY(matchesLogSourceFilter(QStringLiteral("perf"), QStringLiteral("app")));
        QVERIFY(!matchesLogSourceFilter(QStringLiteral("api"), QStringLiteral("app")));
    }

    // filterLogEntries semantics (logs-page-logs.ui.tsx:248-268): level,
    // source, scope and runtime AND together; search is a trimmed,
    // case-insensitive substring of "level scope runtime message JSON(data)".
    void sessionLogFiltersMatchReferenceSemantics()
    {
        using namespace workspace::diagnostics;
        QList<SessionLogEntry> entries;
        SessionLogEntry api;
        api.level = QStringLiteral("error"); api.scope = QStringLiteral("api");
        api.message = QStringLiteral("Request failed GET /streams");
        api.runtime = QStringLiteral("linux");
        api.data = QVariantMap{{QStringLiteral("status"), 500}};
        api.hasData = true;
        entries.append(api);
        SessionLogEntry boot;
        boot.level = QStringLiteral("info"); boot.scope = QStringLiteral("app");
        boot.message = QStringLiteral("Workspace ready");
        boot.runtime = QStringLiteral("linux");
        entries.append(boot);
        SessionLogEntry trace;
        trace.level = QStringLiteral("debug"); trace.scope = QStringLiteral("trace:chat-list");
        trace.message = QStringLiteral("repaint"); trace.runtime = QStringLiteral("linux");
        entries.append(trace);

        const auto pass = filterSessionLogEntries(entries,
            QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("all"),
            QStringLiteral("all"), QString());
        QCOMPARE(pass.size(), qsizetype(3));
        const QStringList levels{QStringLiteral("error"), QStringLiteral("debug"), QStringLiteral("info")};
        for (qsizetype index = 0; index < levels.size(); ++index) {
            const auto only = filterSessionLogEntries(entries, levels.at(index),
                QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("all"), QString());
            QCOMPARE(only.size(), qsizetype(1));
            QCOMPARE(only.at(0).level, levels.at(index));
        }
        const auto sourceApi = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("api"), QStringLiteral("all"), QStringLiteral("all"), QString());
        QCOMPARE(sourceApi.size(), qsizetype(1));
        QCOMPARE(sourceApi.at(0).scope, QStringLiteral("api"));
        const auto sourceApp = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("app"), QStringLiteral("all"), QStringLiteral("all"), QString());
        QCOMPARE(sourceApp.size(), qsizetype(1));
        const auto sourceTrace = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("trace"), QStringLiteral("all"), QStringLiteral("all"), QString());
        QCOMPARE(sourceTrace.size(), qsizetype(1));
        const auto scopeOnly = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("all"), QStringLiteral("app"), QStringLiteral("all"), QString());
        QCOMPARE(scopeOnly.size(), qsizetype(1));
        const auto runtimeMiss = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("windows"), QString());
        QVERIFY(runtimeMiss.isEmpty());
        // Search: case-insensitive, trimmed, matches message and data JSON.
        const auto upper = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("all"),
            QStringLiteral("  REQUEST FAILED  "));
        QCOMPARE(upper.size(), qsizetype(1));
        QCOMPARE(upper.at(0).scope, QStringLiteral("api"));
        const auto viaData = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("all"),
            QStringLiteral("500"));
        QCOMPARE(viaData.size(), qsizetype(1));
        const auto viaScope = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("all"),
            QStringLiteral("trace:"));
        QCOMPARE(viaScope.size(), qsizetype(1));
        // Whitespace-only search trims to "everything".
        const auto blank = filterSessionLogEntries(entries, QStringLiteral("all"),
            QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("all"),
            QStringLiteral("   "));
        QCOMPARE(blank.size(), qsizetype(3));
    }

    // logger.ts redact(): sensitive keys (normalized), sensitive value
    // patterns, and the depth-8 cutoff.
    void sessionLogRedactionMatchesReferenceRules()
    {
        using namespace workspace::diagnostics;
        QVariantMap data;
        data.insert(QStringLiteral("token"), QStringLiteral("abc123"));
        data.insert(QStringLiteral("auth"), QStringLiteral("plain value"));
        data.insert(QStringLiteral("Keep-Me"), QStringLiteral("visible"));
        data.insert(QStringLiteral("authHeader"), QStringLiteral("Basic dXNlcjpwYXNz"));
        data.insert(QStringLiteral("blob"), QString(20, QLatin1Char('A')));
        data.insert(QStringLiteral("short"), QStringLiteral("abcdefghij012345678")); // 19 chars: stays
        data.insert(QStringLiteral("list"), QVariantList{
            QVariantMap{{QStringLiteral("apiKey"), QStringLiteral("x")}}, QStringLiteral("plain")});
        const auto redacted = redactLogValue(data).toMap();
        QCOMPARE(redacted.value(QStringLiteral("token")).toString(), QStringLiteral("[REDACTED]"));
        QCOMPARE(redacted.value(QStringLiteral("auth")).toString(), QStringLiteral("[REDACTED]"));
        QCOMPARE(redacted.value(QStringLiteral("Keep-Me")).toString(), QStringLiteral("visible"));
        QCOMPARE(redacted.value(QStringLiteral("authHeader")).toString(), QStringLiteral("[REDACTED]"));
        QCOMPARE(redacted.value(QStringLiteral("blob")).toString(), QStringLiteral("[REDACTED]"));
        QCOMPARE(redacted.value(QStringLiteral("short")).toString(), QStringLiteral("abcdefghij012345678"));
        const auto list = redacted.value(QStringLiteral("list")).toList();
        QCOMPARE(list.at(0).toMap().value(QStringLiteral("apiKey")).toString(),
            QStringLiteral("[REDACTED]"));
        QCOMPARE(list.at(1).toString(), QStringLiteral("plain"));
        // Depth: the reference nests through depth 8 and stops there.
        QVariant deep = QStringLiteral("deep value");
        for (int level = 0; level < 9; ++level) {
            QVariant nested;
            nested = QVariantMap{{QStringLiteral("next"), deep}};
            deep = nested;
        }
        QVariant walker = redactLogValue(deep);
        int depth = 0;
        while (walker.typeId() == QMetaType::QVariantMap && depth < 20) {
            walker = walker.toMap().value(QStringLiteral("next"));
            ++depth;
        }
        QCOMPARE(walker.toString(), QStringLiteral("[max depth]"));
    }

    // Controller surface: append/clear/clear live signal + filter invokables.
    void controllerSessionLogSurfaceAppendFilterClear()
    {
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QSignalSpy changed(&controller, &NativeController::sessionLogChanged);
        controller.appendSessionLog(QStringLiteral("error"), QStringLiteral("api"),
            QStringLiteral("Request failed GET /streams"),
            QVariantMap{{QStringLiteral("status"), 500}, {QStringLiteral("secret"), QStringLiteral("s3cret")}});
        controller.appendSessionLog(QStringLiteral("info"), QStringLiteral("app"),
            QStringLiteral("Workspace ready"));
        QCOMPARE(changed.count(), 2);
        const auto entries = controller.diagnosticsSessionLogEntries();
        QCOMPARE(entries.size(), qsizetype(2));
        // Newest first: the "Workspace ready" line is the most recent entry.
        const auto first = entries.first().toMap();
        QCOMPARE(first.value(QStringLiteral("level")).toString(), QStringLiteral("info"));
        QCOMPARE(first.value(QStringLiteral("scope")).toString(), QStringLiteral("app"));
        const auto second = entries.at(1).toMap();
        QCOMPARE(second.value(QStringLiteral("level")).toString(), QStringLiteral("error"));
        QCOMPARE(second.value(QStringLiteral("scope")).toString(), QStringLiteral("api"));
        QVERIFY(second.contains(QStringLiteral("runtime")));
        const auto filtered = controller.diagnosticsSessionLogsFiltered(
            QStringLiteral("error"), QStringLiteral("all"), QStringLiteral("all"),
            QStringLiteral("all"), QString());
        QCOMPARE(filtered.size(), qsizetype(1));
        const auto searched = controller.diagnosticsSessionLogsFiltered(
            QStringLiteral("all"), QStringLiteral("all"), QStringLiteral("all"),
            QStringLiteral("all"), QStringLiteral("ready"));
        QCOMPARE(searched.size(), qsizetype(1));
        controller.clearSessionLog();
        QCOMPARE(changed.count(), 3);
        QVERIFY(controller.diagnosticsSessionLogEntries().isEmpty());
    }

    // logs-export.lib.ts: UTC filename shape, 2-space pretty body, and the
    // round-trip with redacted data; the native save path writes the file.
    void sessionLogExportFilenamesAndRoundTrip()
    {
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        controller.appendSessionLog(QStringLiteral("error"), QStringLiteral("api"),
            QStringLiteral("Request failed"),
            QVariantMap{{QStringLiteral("status"), 500}, {QStringLiteral("token"), QStringLiteral("top-secret")}});
        controller.appendSessionLog(QStringLiteral("info"), QStringLiteral("app"),
            QStringLiteral("Workspace ready"));
        const QString name = controller.diagnosticsLogsExportFilename();
        QVERIFY2(QRegularExpression(QStringLiteral("^workspace-logs-[0-9]{8}-[0-9]{6}[.]json$")).match(name).hasMatch(),
            qPrintable(name));
        const QString json = controller.diagnosticsLogsExportJson();
        QVERIFY2(json.contains(QStringLiteral("\n    \"level\"")), qPrintable(json.left(120)));
        QVERIFY(json.contains(QStringLiteral("[REDACTED]")));
        QVERIFY(!json.contains(QStringLiteral("top-secret")));
        const auto parsed = QJsonDocument::fromJson(json.toUtf8()).array();
        QCOMPARE(parsed.size(), 2);
        // Newest first, like getLogHistory: the "Workspace ready" line leads.
        const auto first = parsed.at(0).toObject();
        QCOMPARE(first.value(QStringLiteral("level")).toString(), QStringLiteral("info"));
        QCOMPARE(first.value(QStringLiteral("message")).toString(), QStringLiteral("Workspace ready"));
        QVERIFY(!first.contains(QStringLiteral("data")));
        const auto second = parsed.at(1).toObject();
        QCOMPARE(second.value(QStringLiteral("level")).toString(), QStringLiteral("error"));
        QCOMPARE(second.value(QStringLiteral("scope")).toString(), QStringLiteral("api"));
        QVERIFY(second.contains(QStringLiteral("data")));
        QCOMPARE(second.value(QStringLiteral("data")).toObject().value(QStringLiteral("token")).toString(),
            QStringLiteral("[REDACTED]"));
        QCOMPARE(second.value(QStringLiteral("data")).toObject().value(QStringLiteral("status")).toInt(), 500);
        // exportSessionLog writes the snapshot JSON to a picked path (the
        // FileDialog SaveFile flow passes its selected file through).
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto target = directory.filePath(controller.diagnosticsLogsExportFilename());
        QVERIFY(controller.exportSessionLog(target));
        QFile file(target);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), json);
        // A file:// URL is accepted as well (QML FileDialog selectedFile).
        const auto urlTarget = directory.filePath(QStringLiteral("copy.json"));
        QVERIFY(controller.exportSessionLog(QUrl::fromLocalFile(urlTarget).toString()));
        QVERIFY(QFile::exists(urlTarget));
        // An empty buffer exports nothing and refuses to write.
        controller.clearSessionLog();
        QVERIFY(controller.diagnosticsLogsExportJson().isEmpty());
        QVERIFY(!controller.exportSessionLog(directory.filePath(QStringLiteral("never.json"))));
    }

    // Live producers only log real state: an answered HTTP 500 through the
    // request wrapper lands as an api-scope error entry with method/path/status.
    void sessionLogProducerLogsApiFailures()
    {
        Fixture f; QVERIFY(f.start());
        const auto before = f.controller.diagnosticsSessionLogEntries().size();
        f.streams = [](const Request &) -> Response {
            return Response{QJsonArray{}, 500};
        };
        f.controller.reloadSnapshot();
        QTRY_VERIFY_WITH_TIMEOUT(
            f.controller.diagnosticsSessionLogEntries().size() > before, 5000);
        const auto entries = f.controller.diagnosticsSessionLogEntries();
        bool sawApiError = false;
        for (const auto &value : entries) {
            const auto entry = value.toMap();
            if (entry.value(QStringLiteral("scope")).toString() == QLatin1String("api")
                && entry.value(QStringLiteral("level")).toString() == QLatin1String("error")) {
                sawApiError = true;
                QVERIFY(entry.value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("/streams/")));
                QCOMPARE(entry.value(QStringLiteral("data")).toMap()
                    .value(QStringLiteral("status")).toInt(), 500);
            }
        }
        QVERIFY(sawApiError);
    }

    void diagnosticsReportAndViewerShareHttpErrors()
    {
        Fixture f;
        QVERIFY(f.start());
        f.controller.clearSessionLog();
        f.streams = [](const Request &) -> Response { return Response{QJsonArray{}, 500}; };
        QSignalSpy finished(&f.controller, &NativeController::pingFinished);
        f.controller.pingServer();
        QVERIFY(finished.wait(5000));
        QVERIFY(finished.first().at(0).toBool()); // HTTP 500 still proves reachability
        const auto errors = f.controller.diagnosticsSessionLogsFiltered("error", "api", "all", "all", "");
        QVERIFY(!errors.isEmpty());
        const auto report = QJsonDocument::fromJson(f.controller.diagnosticsConnectionReportJson().toUtf8()).object();
        QCOMPARE(report.value("overallStatus").toString(), QStringLiteral("degraded"));
        const auto recent = report.value("recentErrors").toArray();
        QVERIFY(!recent.isEmpty());
        QCOMPARE(recent.first().toObject().value("message").toString(), errors.first().toMap().value("message").toString());
        f.controller.clearSessionLog();
        const auto logs = f.controller.diagnosticsSnapshot().value("logs").toMap();
        QCOMPARE(logs.value("total").toInt(), 0);
        QVERIFY(logs.value("recentErrors").toList().isEmpty());
    }

#ifdef Q_OS_LINUX
    void sessionLogExportPreservesReportOnFailedWrite()
    {
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.appendSessionLog("info", "app", QString(2048, QLatin1Char('x')));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile report(dir.filePath("report.json"));
        QVERIFY(report.open(QIODevice::WriteOnly));
        const QByteArray previous = "previous report";
        QCOMPARE(report.write(previous), previous.size());
        report.close();
        struct rlimit limits;
        QVERIFY(getrlimit(RLIMIT_FSIZE, &limits) == 0);
        auto capped = limits;
        capped.rlim_cur = 64;
        struct sigaction ignore{}, original{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        QVERIFY(sigaction(SIGXFSZ, &ignore, &original) == 0);
        const auto restoreSignal = qScopeGuard([&] { sigaction(SIGXFSZ, &original, nullptr); });
        QVERIFY(setrlimit(RLIMIT_FSIZE, &capped) == 0);
        const bool success = controller.exportSessionLog(report.fileName());
        const int restored = setrlimit(RLIMIT_FSIZE, &limits);
        QVERIFY(restored == 0);
        QVERIFY(!success);
        QVERIFY(report.open(QIODevice::ReadOnly));
        QCOMPARE(report.readAll(), previous);
        report.close();
        QVERIFY(controller.exportSessionLog(report.fileName()));
        QVERIFY(report.open(QIODevice::ReadOnly));
        QCOMPARE(report.readAll(), controller.diagnosticsLogsExportJson().toUtf8());
    }
#endif

    // Review 4616699148 on #411: transport failures never answer an HTTP
    // status, so the status-gated session log branch skipped them and only
    // the offline transition was recorded (first failure, no request
    // details). The request wrapper must log every transport failure as its
    // own api entry with the method and path (redacted): reference
    // loggedFetch logs the failure per request (logger.ts logApiCall).
    void sessionLogProducerLogsTransportFailuresWithRequestDetails()
    {
        Fixture f; QVERIFY(f.start());
        f.streams = [](const Request &) -> Response {
            Response response;
            response.abortConnection = true; // connection dropped, no HTTP status
            return response;
        };
        const auto transportErrorCount = [&f] {
            int count = 0;
            for (const auto &value : f.controller.diagnosticsSessionLogEntries()) {
                const auto entry = value.toMap();
                if (entry.value(QStringLiteral("scope")).toString() == QLatin1String("api")
                    && entry.value(QStringLiteral("level")).toString() == QLatin1String("error"))
                    ++count;
            }
            return count;
        };
        const auto offlineTransitions = [&f] {
            int count = 0;
            for (const auto &value : f.controller.diagnosticsSessionLogEntries()) {
                const auto entry = value.toMap();
                if (entry.value(QStringLiteral("scope")).toString() == QLatin1String("connection-health")
                    && entry.value(QStringLiteral("message")).toString()
                        == QStringLiteral("Network transport offline"))
                    ++count;
            }
            return count;
        };
        f.controller.reloadSnapshot();
        QTRY_VERIFY_WITH_TIMEOUT(transportErrorCount() > 0, 5000);
        bool shaped = false;
        for (const auto &value : f.controller.diagnosticsSessionLogEntries()) {
            const auto entry = value.toMap();
            if (entry.value(QStringLiteral("scope")).toString() != QLatin1String("api")) continue;
            const auto message = entry.value(QStringLiteral("message")).toString();
            if (!message.contains(QStringLiteral("GET")) || !message.contains(QStringLiteral("/streams/")))
                continue;
            const auto data = entry.value(QStringLiteral("data")).toMap();
            QVERIFY(!data.contains(QStringLiteral("status"))); // no HTTP status to report
            // The path map value may itself land [REDACTED]: a pure
            // base64-alphabet path trips the logger.ts value pattern on
            // purpose (the message keeps the request identity, like the
            // reference where only data is redacted, never the message).
            shaped = data.value(QStringLiteral("method")).toString() == QStringLiteral("GET")
                && data.contains(QStringLiteral("path"))
                && !data.value(QStringLiteral("error")).toString().isEmpty();
        }
        QVERIFY(shaped);
        // Per-request details: the offline transition may interleave with the
        // parallel HTTP answers of the same reload, but every transport
        // failure after the first logs its own api entry even while already
        // offline — the transition entry alone no longer hides them.
        QVERIFY(offlineTransitions() >= 1);
        const auto firstCount = transportErrorCount();
        f.controller.reloadSnapshot();
        QTRY_VERIFY_WITH_TIMEOUT(transportErrorCount() >= firstCount + 1, 5000);
        QVERIFY(offlineTransitions() >= 1);
    }
};

QTEST_MAIN(ControllerDiagnosticsTest)
#include "native-controller-diagnostics-test.moc"
