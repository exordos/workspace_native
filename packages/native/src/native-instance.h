#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <memory>

namespace workspace {

// A second launch activates the existing preview. IPC accepts no routes,
// credentials, commands or arbitrary payloads.
class NativeInstance final : public QObject {
    Q_OBJECT
public:
    enum class Outcome { Primary, Secondary, Failed };
    explicit NativeInstance(QString directory = {}, QObject *parent = nullptr);
    Outcome acquireOrActivate();
    QString error() const { return m_error; }
    bool activationPending() const { return m_activationPending; }

signals:
    void activationRequested();

private:
    void acceptConnections();
    QString m_directory;
    QString m_socket;
    QString m_error;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer m_server;
    bool m_activationPending = false;
};

} // namespace workspace
