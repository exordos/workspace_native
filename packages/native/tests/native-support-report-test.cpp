#include "support/support-report.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_UNIX
#include <QProcess>
#include <csignal>
#include <sys/resource.h>
#endif

using namespace workspace;

class NativeSupportReportTest : public QObject {
    Q_OBJECT
private slots:
    void exportsReviewedBytesOffline()
    {
        SupportReport service;
        const QString privateText = "private-person@example.invalid?token=fixture-private";
        const QVariantMap nested { { "password", privateText }, { "path", "/home/private/private-note.txt" } };
        const QVariantMap counts { { "error", 510 }, { "info", nested }, { "warn", privateText }, { "other", nested } };
        service.prepare(privateText, privateText, privateText, { { "levels", counts }, { "classes", QVariantMap{{"api", 499}, {"realtime", 4}, {privateText, 40}} }, { "requests", 2 }, { "medianMs", 90 }, { "p95Ms", 110 }, { "slowest", nested } });
        QTRY_VERIFY(!service.preview().isEmpty());
        const auto bytes = service.preview().toUtf8();
        QVERIFY(bytes.size() < 65536);
        const auto data = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(data.value("schemaVersion").toInt(), 1);
        QCOMPARE(data.value("eventCounts").toObject().value("error").toInt(), 500);
        QCOMPARE(data["apiLatency"].toObject()["p95Ms"].toInt(), 110);
        const auto classes = data["eventClasses"].toObject();
        QCOMPARE(classes["api"].toInt(), 499);
        QCOMPARE(classes["realtime"].toInt(), 1);
        QVERIFY(!classes.contains(privateText));
        auto env = data.value("environment").toObject();
        QCOMPARE(env.value("display").toString(), "unavailable");
        QVERIFY(!bytes.contains("fixture-private") && !bytes.contains("/home/private"));
        QTemporaryDir directory;
        QFile saved(directory.filePath("report.json"));
        QVERIFY(service.save(QUrl::fromLocalFile(saved.fileName())));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), bytes);
        QVERIFY(!service.save(QUrl("https://example.invalid/report")));
        QVERIFY(!service.save(QUrl::fromLocalFile(directory.filePath("absent/file"))));
        service.cancel();
        QVERIFY(service.preview().isEmpty());
        QVERIFY(!service.save(QUrl::fromLocalFile(saved.fileName())));
    }
    void cancelledCollectionCannotPublishAfterReplacement()
    {
        SupportReport service;
        service.prepare("xcb", "ru", "dark", { { "levels", QVariantMap { { "info", 3 } } } });
        service.cancel();
        service.prepare("wayland", "en", "light", { });
        QTRY_VERIFY(!service.busy());
        auto data = QJsonDocument::fromJson(service.preview().toUtf8()).object();
        QCOMPARE(data["environment"].toObject()["display"].toString(), "wayland");
        QVERIFY(data["eventCounts"].isNull());
        service.cancel();
        QVERIFY(service.preview().isEmpty());
    }
#ifdef Q_OS_UNIX
    void failedWritePreservesExistingFile()
    {
        const char* key = "WORKSPACE_TEST_REPORT_FILE";
        if (qEnvironmentVariableIsSet(key)) {
            // The parent supplies a disposable existing file and a zero write budget.
            SupportReport report;
            report.prepare("xcb", "en", "light", { });
            QTRY_VERIFY(!report.preview().isEmpty());
            QVERIFY(!report.save(QUrl::fromLocalFile(qEnvironmentVariable(key))));
            return;
        }
        QTemporaryDir dir;
        QFile existing(dir.filePath("existing.json"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("existing"), 8);
        existing.close();
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(key, existing.fileName());
        child.setProcessEnvironment(environment);
        child.setChildProcessModifier([] {
            const rlimit limit { 0, 0 };
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
                _exit(125);
            std::signal(SIGXFSZ, SIG_IGN);
        });
        child.start(QCoreApplication::applicationFilePath(), { "failedWritePreservesExistingFile" });
        QVERIFY(child.waitForFinished(10000));
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
            child.readAllStandardOutput().constData());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), "existing");
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList { "existing.json" });
    }
#endif
    void terminalReportContainsBinaryIdentityWithoutAnAccount()
    {
        const auto data = QJsonDocument::fromJson(SupportReport::basicReport()).object();
        QCOMPARE(data["environment"].toObject()["display"].toString(), "headless");
        QVERIFY(data["eventCounts"].isNull());
        QCryptographicHash digest(QCryptographicHash::Sha256);
        QFile executable(QCoreApplication::applicationFilePath());
        QVERIFY(executable.open(QIODevice::ReadOnly));
        QVERIFY(digest.addData(&executable));
        QCOMPARE(data["build"].toObject()["executableSha256"].toString().toLatin1(), digest.result().toHex());
    }
};
QTEST_GUILESS_MAIN(NativeSupportReportTest)
#include "native-support-report-test.moc"
