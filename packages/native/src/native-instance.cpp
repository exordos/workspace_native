#include "native-instance.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>

namespace workspace {

NativeInstance::NativeInstance(QString directory, QObject *parent)
    : QObject(parent), m_directory(std::move(directory)), m_server(this)
{
    if (m_directory.isEmpty()) {
        auto base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!base.isEmpty()) m_directory = base + "/com.exordos.workspace.native.preview";
    }
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(8);
    connect(&m_server, &QLocalServer::newConnection, this, &NativeInstance::acceptConnections);
}

NativeInstance::Outcome NativeInstance::acquireOrActivate()
{
    if (m_server.isListening()) return Outcome::Primary;
    if (m_directory.isEmpty() || QFileInfo(m_directory).isSymLink()
        || !QDir().mkpath(m_directory)
        || !QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        m_error = tr("The private application instance directory is unavailable.");
        return Outcome::Failed;
    }
    m_directory = QFileInfo(m_directory).canonicalFilePath();
    m_lock = std::make_unique<QLockFile>(m_directory + "/instance.lock");
    m_lock->setStaleLockTime(0);
#ifdef Q_OS_WIN
    m_socket = "exordos-workspace-native-preview-" + QString::fromLatin1(
        QCryptographicHash::hash(m_directory.toUtf8(), QCryptographicHash::Sha256).toHex());
#else
    m_socket = m_directory + "/instance.socket";
#endif
    if (!m_lock->tryLock(0)) {
        if (m_lock->error() != QLockFile::LockFailedError) {
            m_error = tr("The application instance lock could not be opened.");
            return Outcome::Failed;
        }
        // Never start another primary or remove a socket while a live process
        // holds the lock, even if that process is temporarily unresponsive.
        QLocalSocket socket;
        socket.setReadBufferSize(4);
        socket.connectToServer(m_socket);
        if (!socket.waitForConnected(1000) || socket.write("activate\n") != 9
            || (socket.bytesToWrite() && !socket.waitForBytesWritten(1000))) {
            m_error = tr("The running application did not confirm activation. Try again shortly.");
            return Outcome::Failed;
        }
        QDeadlineTimer deadline(2000);
        QByteArray reply;
        while (reply.size() < 3 && !deadline.hasExpired()) {
            reply += socket.readAll();
            if (reply.size() >= 3 || !socket.waitForReadyRead(int(deadline.remainingTime()))) break;
        }
        reply += socket.readAll();
        if (reply != "ok\n") {
            m_error = tr("The running application did not confirm activation. Try again shortly.");
            return Outcome::Failed;
        }
        return Outcome::Secondary;
    }
    // The lock proves no cooperating live instance owns this socket. Remove
    // only the stale socket in this preview's private directory.
    QLocalServer::removeServer(m_socket);
    if (!m_server.listen(m_socket)) {
        m_lock->unlock();
        m_error = tr("The private application activation service could not start.");
        return Outcome::Failed;
    }
    m_error.clear();
    return Outcome::Primary;
}

void NativeInstance::release()
{
    m_server.close();
    const auto sockets = m_server.findChildren<QLocalSocket *>();
    for (auto *socket : sockets) socket->abort();
    m_lock.reset();
    m_activationPending = false;
}

void NativeInstance::acceptConnections()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        socket->setParent(&m_server);
        socket->setReadBufferSize(65);
        auto buffer = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        const auto read = [this, socket, buffer, handled] {
            if (*handled) return;
            *buffer += socket->readAll();
            if (buffer->size() > 64) {
                *handled = true;
                socket->abort();
                return;
            }
            if (!buffer->contains('\n')) return;
            *handled = true;
            if (*buffer == "activate\n") {
                socket->write("ok\n");
                socket->flush();
                m_activationPending = true;
                emit activationRequested();
            }
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, socket, read);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(1500, socket, [socket] { socket->abort(); socket->deleteLater(); });
        if (socket->bytesAvailable()) read();
    }
}

} // namespace workspace
