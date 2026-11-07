#include "native-instance.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstdio>

using namespace workspace;

static QString testEndpoint(const QString &directory)
{
    const auto canonical = QFileInfo(directory).canonicalFilePath();
#ifdef Q_OS_WIN
    return "exordos-workspace-native-preview-" + QString::fromLatin1(
        QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex());
#else
    return canonical + "/instance.socket";
#endif
}

static int worker(QCoreApplication &app, const QStringList &arguments)
{
    if (arguments.size() != 4) return 2;
    NativeInstance instance(arguments[3]);
    const auto outcome = instance.acquireOrActivate();
    if (arguments[2] == "secondary") return outcome == NativeInstance::Outcome::Secondary ? 0 : 1;
    if (arguments[2] != "primary" || outcome != NativeInstance::Outcome::Primary) return 2;
    std::puts("ready");
    std::fflush(stdout);
    QTimer::singleShot(10000, &app, &QCoreApplication::quit);
    return app.exec();
}

class NativeInstanceTest final : public QObject {
    Q_OBJECT
private slots:
    void secondProcessActivatesPrimaryAndExits()
    {
        QTemporaryDir directory;
        NativeInstance primary(directory.filePath("instance"));
        QCOMPARE(primary.acquireOrActivate(), NativeInstance::Outcome::Primary);
        QCOMPARE(primary.acquireOrActivate(), NativeInstance::Outcome::Primary);
        QSignalSpy activation(&primary, &NativeInstance::activationRequested);
        QProcess secondary;
        secondary.start(QCoreApplication::applicationFilePath(), {"--worker", "secondary", directory.filePath("instance")});
        QVERIFY(secondary.waitForStarted(2000));
        QTRY_COMPARE_WITH_TIMEOUT(secondary.state(), QProcess::NotRunning, 5000);
        QCOMPARE(secondary.exitCode(), 0);
        QCOMPARE(activation.size(), 1);
        QVERIFY(primary.activationPending());
#ifndef Q_OS_WIN
        const auto permissions = QFileInfo(directory.filePath("instance")).permissions();
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));
#endif
    }

    void crashedProcessReleasesLockAndStaleSocketIsRecovered()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("instance");
        QProcess crashed;
        crashed.start(QCoreApplication::applicationFilePath(), {"--worker", "primary", path});
        QVERIFY(crashed.waitForStarted(2000));
        QVERIFY(crashed.waitForReadyRead(3000));
        QCOMPARE(crashed.readAllStandardOutput().trimmed(), "ready");
        crashed.kill();
        QVERIFY(crashed.waitForFinished(3000));
        NativeInstance recovered(path);
        QCOMPARE(recovered.acquireOrActivate(), NativeInstance::Outcome::Primary);
    }

    void differentNamespacesRemainIndependent()
    {
        QTemporaryDir directory;
        NativeInstance first(directory.filePath("first"));
        NativeInstance second(directory.filePath("second"));
        QCOMPARE(first.acquireOrActivate(), NativeInstance::Outcome::Primary);
        QCOMPARE(second.acquireOrActivate(), NativeInstance::Outcome::Primary);
    }

    void activationAcknowledgementCanArriveInFragments()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("instance");
        QVERIFY(QDir().mkpath(path));
        QLockFile lock(path + "/instance.lock");
        QVERIFY(lock.tryLock(0));
        QLocalServer server;
        QVERIFY(server.listen(testEndpoint(path)));
        connect(&server, &QLocalServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            QVERIFY(socket);
            connect(socket, &QLocalSocket::readyRead, socket, [socket] {
                if (!socket->readAll().contains("activate\n")) return;
                socket->write("o");
                socket->flush();
                QTimer::singleShot(25, socket, [socket] {
                    socket->write("k\n");
                    socket->disconnectFromServer();
                });
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        });
        QProcess secondary;
        secondary.start(QCoreApplication::applicationFilePath(), {"--worker", "secondary", path});
        QVERIFY(secondary.waitForStarted(2000));
        QTRY_COMPARE_WITH_TIMEOUT(secondary.state(), QProcess::NotRunning, 5000);
        QCOMPARE(secondary.exitCode(), 0);
    }

    void malformedLocalTrafficCannotActivate()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("instance");
        NativeInstance primary(path);
        QCOMPARE(primary.acquireOrActivate(), NativeInstance::Outcome::Primary);
        QSignalSpy activation(&primary, &NativeInstance::activationRequested);
        QLocalSocket socket;
        socket.connectToServer(testEndpoint(path));
        QVERIFY(socket.waitForConnected(1000));
        socket.write("activate\nunknown-command\n");
        QVERIFY(socket.waitForBytesWritten(1000));
        QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
        QCOMPARE(activation.size(), 0);
        QVERIFY(!primary.activationPending());
    }

    void unavailableDirectoryFailsWithoutCreatingOtherNamespace()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("not-a-directory"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        NativeInstance instance(file.fileName());
        QCOMPARE(instance.acquireOrActivate(), NativeInstance::Outcome::Failed);
        QVERIFY(!instance.error().isEmpty());
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--worker") return worker(app, app.arguments());
    NativeInstanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "native-instance-test.moc"
