#include "native-desktop.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTest>
#include <QWindow>

using namespace workspace;

class NativeDesktopTest final : public QObject {
    Q_OBJECT
private slots:
    void closeHidesToAnAvailableTrayAndActivationCanRestoreTheSameWindow()
    {
        QWindow window;
        window.show();
        NativeDesktop desktop(&window, nullptr, [] { return true; });
        QSignalSpy activate(&desktop, &NativeDesktop::activateRequested);
        QSignalSpy quit(&desktop, &NativeDesktop::quitRequested);
        connect(&desktop, &NativeDesktop::activateRequested, &window, &QWindow::show);
        QVERIFY(window.isVisible());
        QVERIFY(!window.close());
        QVERIFY(!window.isVisible());
        QCOMPARE(quit.count(), 0);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray);
        QVERIFY(tray->isVisible());
        emit tray->activated(QSystemTrayIcon::Trigger);
        QCOMPARE(activate.count(), 1);
        QVERIFY(window.isVisible());
        emit tray->activated(QSystemTrayIcon::Context);
        QCOMPARE(activate.count(), 1);
    }

    void unavailableOrDisappearingTrayDoesNotTrapTheApplication()
    {
        bool available = false;
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [&available] { return available; });
        window.show();
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
        available = true;
        window.show();
        QVERIFY(!window.close());
        available = false;
        window.show();
        QVERIFY(window.close());
    }

    void explicitQuitFromAHiddenWindowIsDeliveredOnceAndCannotHideAgain()
    {
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return true; });
        QSignalSpy quit(&desktop, &NativeDesktop::quitRequested);
        QSignalSpy activate(&desktop, &NativeDesktop::activateRequested);
        window.show();
        QVERIFY(!window.close());
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->contextMenu());
        auto *action = tray->contextMenu()->findChild<QAction *>("trayQuit");
        QVERIFY(action);
        action->trigger();
        action->trigger();
        QCOMPARE(quit.count(), 1);
        QVERIFY(!tray->isVisible());
        emit tray->activated(QSystemTrayIcon::Trigger);
        QCOMPARE(activate.count(), 0);
        auto *messenger = tray->contextMenu()->findChild<QAction *>("trayMessenger");
        QVERIFY(messenger);
        messenger->trigger();
        QCOMPARE(activate.count(), 0);
        window.show();
        QVERIFY(window.close());
    }

    void trayUsesSourceIconsAndKeepsUnsupportedSectionsExplicit()
    {
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return false; });
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->contextMenu());
        auto *menu = tray->contextMenu();
        QCOMPARE(menu->actions().first()->text(), QString("Workspace"));
        QVERIFY(!menu->actions().first()->isEnabled());
        const auto normal = tray->icon().pixmap(32, 32).toImage();
        QVERIFY(!normal.isNull());
#ifdef Q_OS_MACOS
        const QString base = ":/workspace/tray/tray-icon-mac";
#elif defined(Q_OS_LINUX)
        const QString base = ":/workspace/tray/tray-icon-linux";
#else
        const QString base = ":/workspace/tray/tray-icon";
#endif
        QCOMPARE(normal, QIcon(base + ".png").pixmap(32, 32).toImage());
        desktop.setUnreadCount(12);
        QCOMPARE(menu->actions().first()->text(), QString("Workspace (12)"));
        const auto unread = tray->icon().pixmap(32, 32).toImage();
        QVERIFY(!unread.isNull());
        QVERIFY(unread != normal);
        QCOMPARE(unread, QIcon(base + "-unread.png").pixmap(32, 32).toImage());
        desktop.setUnreadCount(-1);
        QCOMPARE(menu->actions().first()->text(), QString("Workspace"));
        for (const auto *name : {"trayCalendar", "trayMail"}) {
            auto *action = menu->findChild<QAction *>(name);
            QVERIFY(action && !action->isEnabled());
            QVERIFY(action->toolTip().contains("not available"));
        }
        QSignalSpy activate(&desktop, &NativeDesktop::activateRequested);
        auto *messenger = menu->findChild<QAction *>("trayMessenger");
        QVERIFY(messenger && messenger->isEnabled());
        messenger->trigger();
        QCOMPARE(activate.count(), 1);
    }

    void desktopQuitShortcutRequestsTheSameGracefulShutdown()
    {
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return true; });
        QSignalSpy quit(&desktop, &NativeDesktop::quitRequested);
        window.show();
        window.requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), &window);
#ifdef Q_OS_MACOS
        QTest::keyClick(&window, Qt::Key_Q, Qt::MetaModifier);
#else
        QTest::keyClick(&window, Qt::Key_Q, Qt::ControlModifier);
#endif
        QTRY_COMPARE(quit.count(), 1);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    NativeDesktopTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "native-desktop-test.moc"
