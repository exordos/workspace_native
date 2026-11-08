#pragma once

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QAction;
class QMenu;
class QShortcut;
class QSystemTrayIcon;
class QWindow;

namespace workspace {

// The messenger remains Qt Quick. Widgets is used only for the OS tray menu.
class NativeDesktop final : public QObject {
    Q_OBJECT
public:
    explicit NativeDesktop(QWindow *window, QObject *parent = nullptr,
                           std::function<bool()> trayAvailable = {});
    ~NativeDesktop() override;
    void setUnreadCount(qint64 count);
    void refreshLabels();
    void requestQuit();

signals:
    void activateRequested();
    void quitRequested();
    void checkForUpdatesRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPointer<QWindow> m_window;
    std::function<bool()> m_trayAvailable;
    std::unique_ptr<QMenu> m_menu;
    QSystemTrayIcon *m_tray = nullptr;
    QPointer<QShortcut> m_quitShortcut;
    QAction *m_header = nullptr;
    QAction *m_messenger = nullptr;
    QAction *m_calendar = nullptr;
    QAction *m_mail = nullptr;
    QAction *m_quit = nullptr;
    QAction *m_updates = nullptr;
    qint64 m_unread = 0;
    bool m_quitting = false;
};

} // namespace workspace
