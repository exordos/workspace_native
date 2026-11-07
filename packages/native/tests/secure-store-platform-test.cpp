#include "session-vault.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <cstdio>

using namespace workspace;

// This opt-in test uses an actual OS store and only its unique disposable key.
// Test material travels over a private child stdin pipe, never arguments/files.
static int worker(QCoreApplication &app, const QStringList &arguments)
{
    if (arguments.size() != 4 || !arguments[3].startsWith("cassi-native-acceptance-")) return 2;
    const auto operation = arguments[2];
    const auto key = arguments[3];
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    auto expected = input.readAll();
    KeychainSessionSecretStore store;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] { app.exit(3); });
    deadline.start(15000);
    QTimer::singleShot(0, &store, [&] {
        using Error = SessionSecretStore::Error;
        if (operation == "write" && expected.size() == 32) {
            store.write(key, expected, [&](Error error) { app.exit(error == Error::None ? 0 : 10 + int(error)); });
        } else if (operation == "read" && expected.size() == 32) {
            store.read(key, [&](QByteArray actual, Error error) {
                const auto matches = error == Error::None && actual == expected;
                actual.fill(0);
                app.exit(matches ? 0 : error == Error::None ? 5 : 10 + int(error));
            });
        } else if (operation == "delete" && expected.isEmpty()) {
            store.remove(key, [&](Error error) {
                app.exit(error == Error::None || error == Error::NotFound ? 0 : 10 + int(error));
            });
        } else if (operation == "missing" && expected.isEmpty()) {
            store.read(key, [&](QByteArray actual, Error error) {
                const auto missing = error == Error::NotFound && actual.isEmpty();
                actual.fill(0);
                app.exit(missing ? 0 : error == Error::None ? 7 : 10 + int(error));
            });
        } else {
            app.exit(2);
        }
    });
    const auto result = app.exec();
    expected.fill(0);
    return result;
}

class SecureStorePlatformTest final : public QObject {
    Q_OBJECT
    QString m_key;
    bool m_writeAttempted = false;

    bool run(const QString &operation, const QByteArray &material = {})
    {
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"--worker", operation, m_key});
        child.start();
        if (!child.waitForStarted(5000)) return false;
        if (!material.isEmpty()) child.write(material);
        child.closeWriteChannel();
        if (!child.waitForFinished(20000)) {
            child.kill();
            child.waitForFinished(5000);
            return false;
        }
        // Backend diagnostics can contain identifying metadata. Report only the
        // stage/exit status; no child output or secret values enter test logs.
        const auto success = child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
        if (!success) qWarning("Secure-store stage %s exited with status %d", qPrintable(operation), child.exitCode());
        return success;
    }

private slots:
    void realSecureStoreSurvivesProcessRestartAndRotation()
    {
        m_key = "cassi-native-acceptance-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto original = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        auto rotated = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        QVERIFY2(run("missing"), "OS store is unavailable, denied, or key absence was not confirmed");
        m_writeAttempted = true;
        QVERIFY2(run("write", original), "Actual secure-store write was not confirmed");
        QVERIFY2(run("read", original), "A new process could not read the exact saved bytes");
        QVERIFY2(run("write", rotated), "Actual secure-store rotation was not confirmed");
        QVERIFY2(run("read", rotated), "A new process did not read the rotated bytes");
        original.fill(0);
        rotated.fill(0);
        QVERIFY2(run("delete"), "Test-owned secure-store cleanup failed");
        QVERIFY2(run("missing"), "Deleted test key is still readable");
    }

    void cleanupTestCase()
    {
        if (m_key.isEmpty() || !m_writeAttempted) return;
        const bool removed = run("delete");
        const bool absent = run("missing");
        if (!removed || !absent)
            qWarning("Cleanup is unconfirmed for test-owned key %s", qPrintable(m_key));
        QVERIFY2(removed && absent, "Test-owned secure-store key cleanup is unconfirmed");
    }
};

int main(int argc, char **argv)
{
    QCoreApplication::setOrganizationName("Exordos");
    QCoreApplication::setApplicationName("Workspace Native Secure Store Acceptance");
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--worker") return worker(app, app.arguments());
    SecureStorePlatformTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "secure-store-platform-test.moc"
