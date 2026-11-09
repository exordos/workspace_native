#include "session-vault.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
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
    QString m_dbusExecutable;
    QString m_keyringExecutable;
    QString m_busAddress;
    QString m_harnessError;
    QProcess *m_privateBus = nullptr;
    QProcess *m_keyringDaemon = nullptr;
    int m_probeCounter = 0;

    bool run(const QString &key, const QString &operation, const QByteArray &material = {},
             const QProcessEnvironment *environment = nullptr)
    {
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"--worker", operation, key});
        if (environment) child.setProcessEnvironment(*environment);
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

    bool startPrivateBus()
    {
        auto *bus = new QProcess(this);
        bus->setProgram(m_dbusExecutable);
        bus->setArguments({"--session", "--print-address=1", "--nofork"});
        bus->start();
        QString address;
        if (bus->waitForStarted(5000) && bus->waitForReadyRead(5000))
            address = QString::fromLatin1(bus->readLine()).trimmed();
        if (!address.startsWith("unix:")) {
            m_harnessError = "The private session bus did not report an address.";
            bus->kill();
            bus->waitForFinished(2000);
            bus->deleteLater();
            return false;
        }
        m_busAddress = address;
        m_privateBus = bus;
        return true;
    }

    bool startKeyringDaemon(const QProcessEnvironment &environment)
    {
        auto *daemon = new QProcess(this);
        daemon->setProgram(m_keyringExecutable);
        daemon->setArguments({"--foreground", "--unlock", "--components=secrets"});
        daemon->setProcessEnvironment(environment);
        daemon->start();
        if (!daemon->waitForStarted(5000)) {
            m_harnessError = "The private keyring daemon did not start: " + daemon->errorString();
            daemon->deleteLater();
            return false;
        }
        // Provision the profile the way PAM does at graphical login: unlock (or
        // on first creation, protect) the login keyring with the disposable
        // harness passphrase, so storage never depends on an interactive prompt.
        daemon->write("disposable-harness-passphrase\n");
        daemon->closeWriteChannel();
        // Flush now: the harness polls with sleeps and does not spin an event
        // loop, so QProcess would otherwise keep the pipe open and the daemon
        // would wait forever for its provisioning passphrase.
        if (!daemon->waitForBytesWritten(5000)) {
            m_harnessError = "The keyring daemon did not accept its provisioning passphrase.";
            daemon->kill();
            daemon->waitForFinished(2000);
            daemon->deleteLater();
            return false;
        }
        m_keyringDaemon = daemon;
        return true;
    }

    void stopKeyringDaemon()
    {
        if (!m_keyringDaemon) return;
        m_keyringDaemon->terminate();
        if (!m_keyringDaemon->waitForFinished(10000)) {
            m_keyringDaemon->kill();
            m_keyringDaemon->waitForFinished(5000);
        }
        m_keyringDaemon->deleteLater();
        m_keyringDaemon = nullptr;
    }

    bool secretServicePublished()
    {
        const auto name = QStringLiteral("secure-store-private-%1").arg(m_probeCounter++);
        const auto connection = QDBusConnection::connectToBus(m_busAddress, name);
        const bool connected = connection.isConnected();
        const auto published = connected
            && connection.interface()->isServiceRegistered(QStringLiteral("org.freedesktop.secrets"));
        QDBusConnection::disconnectFromBus(name);
        return published;
    }

    bool waitForSecretService()
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 15000) {
            if (secretServicePublished()) return true;
            QThread::msleep(150);
        }
        return false;
    }

    QStringList keyringFiles(const QString &profileHome) const
    {
        QDir directory(profileHome + "/.local/share/keyrings");
        QStringList files;
        const auto entries = directory.entryList({"*.keyring"}, QDir::Files);
        for (const auto &entry : entries) files.append(directory.filePath(entry));
        return files;
    }

    bool relocateKeyringFiles(const QString &profileHome, bool restore) const
    {
        QDir source(restore ? profileHome + "/keyring-hold" : profileHome + "/.local/share/keyrings");
        QDir target(restore ? profileHome + "/.local/share/keyrings" : profileHome + "/keyring-hold");
        if (!target.mkpath(".")) return false;
        const auto entries = source.entryList({"*.keyring"}, QDir::Files);
        for (const auto &entry : entries) {
            QFile::remove(target.filePath(entry));
            if (!QFile::rename(source.filePath(entry), target.filePath(entry))) return false;
        }
        return true;
    }

    void stopHarness()
    {
        stopKeyringDaemon();
        if (m_privateBus) {
            m_privateBus->terminate();
            if (!m_privateBus->waitForFinished(5000)) {
                m_privateBus->kill();
                m_privateBus->waitForFinished(2000);
            }
            m_privateBus->deleteLater();
            m_privateBus = nullptr;
        }
    }

private slots:
    void initTestCase()
    {
        m_dbusExecutable = QStandardPaths::findExecutable("dbus-daemon");
        m_keyringExecutable = QStandardPaths::findExecutable("gnome-keyring-daemon");
    }

    void realSecureStoreSurvivesProcessRestartAndRotation()
    {
        m_key = "cassi-native-acceptance-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto original = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        auto rotated = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        QVERIFY2(run(m_key, "missing"), "OS store is unavailable, denied, or key absence was not confirmed");
        m_writeAttempted = true;
        QVERIFY2(run(m_key, "write", original), "Actual secure-store write was not confirmed");
        QVERIFY2(run(m_key, "read", original), "A new process could not read the exact saved bytes");
        QVERIFY2(run(m_key, "write", rotated), "Actual secure-store rotation was not confirmed");
        QVERIFY2(run(m_key, "read", rotated), "A new process did not read the rotated bytes");
        original.fill(0);
        rotated.fill(0);
        QVERIFY2(run(m_key, "delete"), "Test-owned secure-store cleanup failed");
        QVERIFY2(run(m_key, "missing"), "Deleted test key is still readable");
    }

    // Regular desktop profile acceptance: a persistent, on-disk GNOME Keyring
    // profile started on a private session bus so the user's own desktop
    // keyring is never touched. The token must survive a restart of the keyring
    // provider daemon - that is what on-disk persistence means - and a deliberate
    // falsification control proves reads come from disk instead of daemon memory.
    void persistentProfileSurvivesKeyringDaemonRestart()
    {
        if (m_dbusExecutable.isEmpty() || m_keyringExecutable.isEmpty())
            QSKIP("A regular desktop Secret Service provider (dbus-daemon + gnome-keyring-daemon) is not installed");
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        const auto runDirectory = profile.path() + "/run";
        QVERIFY(QDir().mkpath(runDirectory));
        QVERIFY(QFile::setPermissions(runDirectory,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QVERIFY2(startPrivateBus(), qPrintable(m_harnessError));
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOME", profile.path());
        environment.insert("XDG_RUNTIME_DIR", runDirectory);
        environment.insert("DBUS_SESSION_BUS_ADDRESS", m_busAddress);
        environment.remove("XDG_DATA_HOME");
        environment.remove("XDG_CONFIG_HOME");
        environment.remove("XDG_CONFIG_DIR");
        environment.remove("GNOME_KEYRING_CONTROL");
        environment.remove("SSH_AUTH_SOCK");
        QVERIFY2(startKeyringDaemon(environment), qPrintable(m_harnessError));
        QVERIFY2(waitForSecretService(), "The private keyring daemon did not publish org.freedesktop.secrets");
        const auto key = "cassi-native-acceptance-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto original = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        auto rotated = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        const auto restartDaemon = [this, &environment] {
            stopKeyringDaemon();
            return startKeyringDaemon(environment) && waitForSecretService();
        };
        QVERIFY2(run(key, "missing", {}, &environment), "The fresh keyring profile already held the test key");
        QVERIFY2(run(key, "write", original, &environment), "The regular-profile Secret Service did not confirm the token write");
        QVERIFY2(!keyringFiles(profile.path()).isEmpty(), "The keyring profile never wrote a durable on-disk keyring file");
        QVERIFY2(run(key, "read", original, &environment), "A new process could not read the exact saved bytes");
        QVERIFY2(restartDaemon(), "The keyring daemon did not come back after restart");
        QVERIFY2(run(key, "read", original, &environment), "The token was not durable across a keyring daemon restart");
        QVERIFY2(run(key, "write", rotated, &environment), "Regular-profile token rotation was not confirmed");
        QVERIFY2(restartDaemon(), "The keyring daemon did not come back after restart");
        QVERIFY2(run(key, "read", rotated, &environment), "The rotated token was not durable across a keyring daemon restart");
        // Falsification control: with the same profile but its on-disk keyring
        // files moved aside, the token must be gone. A read served from
        // non-durable state would survive this; a durable store must not.
        stopKeyringDaemon();
        QVERIFY2(relocateKeyringFiles(profile.path(), false), "On-disk keyring files could not be moved aside");
        QVERIFY2(restartDaemon(), "The keyring daemon did not come back after restart");
        QVERIFY2(run(key, "missing", {}, &environment), "The token was served from non-durable state instead of the on-disk keyring");
        stopKeyringDaemon();
        QVERIFY2(relocateKeyringFiles(profile.path(), true), "On-disk keyring files could not be restored");
        QVERIFY2(restartDaemon(), "The keyring daemon did not come back after restart");
        QVERIFY2(run(key, "read", rotated, &environment), "The token did not return with the restored on-disk keyring files");
        QVERIFY2(run(key, "delete", {}, &environment), "Test cleanup on the private profile was not confirmed");
        QVERIFY2(run(key, "missing", {}, &environment), "The deleted token remained readable");
        original.fill(0);
        rotated.fill(0);
        stopHarness();
    }

    void cleanupTestCase()
    {
        stopHarness();
        if (m_key.isEmpty() || !m_writeAttempted) return;
        const bool removed = run(m_key, "delete");
        const bool absent = run(m_key, "missing");
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
