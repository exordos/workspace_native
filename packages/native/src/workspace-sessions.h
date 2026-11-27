#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace workspace {

class NativeController;
class NotificationTransport;
class PowerWatcher;
class RealtimeCursorStore;
class Session;
class SessionVault;
struct SavedAccount;

// Owns the authenticated runtimes. The window retains one controller and
// borrows the selected runtime's verified access without owning its renewal.
class WorkspaceSessions final : public QObject {
    Q_OBJECT
public:
    WorkspaceSessions(Session &windowSession, NativeController &window,
                      SessionVault &vault, RealtimeCursorStore &cursors);
    ~WorkspaceSessions() override;

    void start();
    void setNotificationTransport(NotificationTransport *transport);
    void setPowerWatcher(PowerWatcher *watcher);
    void setWindowFocused(bool focused);
    void clickNotification(const QString &tag);

signals:
    void activationRequested();
    void soundRequested();
    void eventReceived(const QJsonObject &event);

private:
    struct Runtime;
    struct Destination { QString account, message; };

    Runtime *ensure(const QString &id);
    Runtime *ensure(const SavedAccount &account);
    void reconcile();
    void select(const QString &id);
    void takeOver(Runtime &runtime);
    void authenticated(Runtime &runtime);
    void present(Runtime &runtime);
    void updateUnread(const Runtime &runtime);
    void openDestination();
    bool isPresented(const Runtime &runtime) const;

    Session &m_windowSession;
    NativeController &m_window;
    SessionVault &m_vault;
    RealtimeCursorStore &m_cursors;
    NotificationTransport *m_transport = nullptr;
    PowerWatcher *m_powerWatcher = nullptr;
    bool m_windowFocused = true;
    QHash<QString, Runtime *> m_runtimes;
    QString m_selected;
    Destination m_destination;
};

} // namespace workspace
