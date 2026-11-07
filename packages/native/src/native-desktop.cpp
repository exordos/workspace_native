#include "native-desktop.h"

#include <QAction>
#include <QCloseEvent>
#include <QIcon>
#include <QMenu>
#include <QResource>
#include <QShortcut>
#include <QSystemTrayIcon>
#include <QWindow>

static void initializeDesktopResources()
{
    Q_INIT_RESOURCE(native_tray);
}

namespace workspace {

NativeDesktop::NativeDesktop(QWindow *window, QObject *parent, std::function<bool()> trayAvailable)
    : QObject(parent), m_window(window),
      m_trayAvailable(trayAvailable ? std::move(trayAvailable) : QSystemTrayIcon::isSystemTrayAvailable),
      m_menu(std::make_unique<QMenu>()), m_tray(new QSystemTrayIcon(this))
{
    initializeDesktopResources();
    m_header = m_menu->addAction(QString{});
    m_header->setEnabled(false);
    m_menu->addSeparator();
    m_messenger = m_menu->addAction(QString{});
    m_messenger->setObjectName("trayMessenger");
    m_calendar = m_menu->addAction(QString{});
    m_calendar->setObjectName("trayCalendar");
    m_mail = m_menu->addAction(QString{});
    m_mail->setObjectName("trayMail");
    // These application sections still need their native implementation.
    m_calendar->setEnabled(false);
    m_mail->setEnabled(false);
    m_menu->addSeparator();
    m_quit = m_menu->addAction(QString{});
    m_quit->setObjectName("trayQuit");
    m_tray->setContextMenu(m_menu.get());
    connect(m_messenger, &QAction::triggered, this, [this] {
        if (!m_quitting) emit activateRequested();
    });
    connect(m_quit, &QAction::triggered, this, &NativeDesktop::requestQuit);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (!m_quitting && (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick))
            emit activateRequested();
    });
    if (m_window) {
        m_window->installEventFilter(this);
        m_quitShortcut = new QShortcut(QKeySequence::Quit, m_window);
        auto bindings = QKeySequence::keyBindings(QKeySequence::Quit);
#ifdef Q_OS_MACOS
        const QKeySequence desktopQuit(Qt::META | Qt::Key_Q);
#else
        const QKeySequence desktopQuit(Qt::CTRL | Qt::Key_Q);
#endif
        // Generic Qt platform themes can map Quit only to the hardware Exit
        // key. Preserve the source desktop's conventional keyboard command.
        if (!bindings.contains(desktopQuit)) bindings.append(desktopQuit);
        m_quitShortcut->setKeys(bindings);
        // QApplication also owns the native tray's QWidget menu. Application
        // scope keeps Quit available across that menu and the Qt Quick window.
        m_quitShortcut->setContext(Qt::ApplicationShortcut);
        connect(m_quitShortcut, &QShortcut::activated, this, &NativeDesktop::requestQuit);
    }
    refreshLabels();
    m_tray->show();
}

NativeDesktop::~NativeDesktop()
{
    m_tray->hide();
    m_tray->setContextMenu(nullptr);
    delete m_quitShortcut.data();
}

void NativeDesktop::setUnreadCount(qint64 count)
{
    count = qMax<qint64>(0, count);
    if (count == m_unread) return;
    m_unread = count;
    refreshLabels();
}

void NativeDesktop::refreshLabels()
{
    const auto title = tr("Workspace");
    m_header->setText(m_unread > 0 ? title + " (" + QString::number(m_unread) + ')' : title);
    m_messenger->setText(tr("Messenger"));
    m_calendar->setText(tr("Calendar"));
    m_calendar->setToolTip(tr("Calendar is not available in this native preview"));
    m_mail->setText(tr("Mail"));
    m_mail->setToolTip(tr("Mail is not available in this native preview"));
    m_quit->setText(tr("Quit"));
    m_tray->setToolTip(title);
    const auto suffix = m_unread > 0 ? QStringLiteral("-unread.png") : QStringLiteral(".png");
#if defined(Q_OS_MACOS)
    const auto base = QStringLiteral(":/workspace/tray/tray-icon-mac");
#elif defined(Q_OS_LINUX)
    const auto base = QStringLiteral(":/workspace/tray/tray-icon-linux");
#else
    const auto base = QStringLiteral(":/workspace/tray/tray-icon");
#endif
    m_tray->setIcon(QIcon(base + suffix));
}

void NativeDesktop::requestQuit()
{
    if (m_quitting) return;
    m_quitting = true;
    m_tray->hide();
    // main() owns the asynchronous cache shutdown and eventual application quit.
    emit quitRequested();
}

bool NativeDesktop::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && event->type() == QEvent::Close && !m_quitting
        && m_trayAvailable() && m_tray->isVisible() && !m_tray->icon().isNull()) {
        // Without a usable tray, closing must remain a way to exit the app.
        static_cast<QCloseEvent *>(event)->ignore();
        m_window->hide();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace workspace
