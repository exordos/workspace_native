#include "native-desktop.h"
#include "native-deeplink.h"

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
    void updateActionRequestsOneCheckAndStopsAfterQuit()
    {
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return false; });
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->contextMenu());
        auto *action = tray->contextMenu()->findChild<QAction *>("trayCheckForUpdates");
        QVERIFY(action && action->isEnabled());
        QSignalSpy checks(&desktop, &NativeDesktop::checkForUpdatesRequested);
        action->trigger();
        QCOMPARE(checks.count(), 1);
        desktop.requestQuit();
        action->trigger();
        QCOMPARE(checks.count(), 1);
    }

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

    void deepLinkAcceptsEveryWhitelistedRouteShape()
    {
        const QString uuid = "66666666-6666-4666-8666-666666666666";
        const QString upperUuid = "66666666-6666-4666-8666-66666666666A";
        const QList<QPair<QString, QString>> accepted = {
            {"ew://open/", "/"},
            {"ew://open", "/"},
            {"ew://open/calendar", "/calendar"},
            {"ew://open/calendar/", "/calendar/"},
            {"ew://open/mail", "/mail"},
            {"ew://open/calls?tab=1", "/calls?tab=1"},
            {"ew://open/licenses#top", "/licenses#top"},
            {"ew://open/settings", "/settings"},
            {"ew://open/logs", "/logs"},
            {"ew://open/org/acme", "/org/acme"},
            {"ew://open/org/acme/", "/org/acme/"},
            {"ew://open/org/acme/settings?x=1#h", "/org/acme/settings?x=1#h"},
            {"ew://open/org/acme/project/p1/messenger", "/org/acme/project/p1/messenger"},
            {"ew://open/org/acme/project/p1/inbox", "/org/acme/project/p1/inbox"},
            {"ew://open/org/acme/project/p1/feed", "/org/acme/project/p1/feed"},
            {"ew://open/org/acme/project/p1/activity/anything", "/org/acme/project/p1/activity/anything"},
            {"ew://open/org/acme/project/p1/message/" + uuid, "/org/acme/project/p1/message/" + uuid},
            {"ew://open/org/acme/project/p1/message/" + upperUuid, "/org/acme/project/p1/message/" + upperUuid},
            {"ew://open/org/acme/project/p1/message/" + uuid + "?focused=1",
                "/org/acme/project/p1/message/" + uuid + "?focused=1"},
            {"ew://open/org/acme/project/p1/stream/aaaaaaaa", "/org/acme/project/p1/stream/aaaaaaaa"},
            {"ew://open/org/acme/project/p1/stream/aaaaaaaa/topic/bbbbbbbb",
                "/org/acme/project/p1/stream/aaaaaaaa/topic/bbbbbbbb"},
            {"ew://open/org/acme/project/p1/stream/aaaaaaaa/topic/bbbbbbbb/?x=1#y",
                "/org/acme/project/p1/stream/aaaaaaaa/topic/bbbbbbbb/?x=1#y"},
            {"EW://open/calendar", "/calendar"},
            {"ew://OPEN/calendar", "/calendar"},
            {"ew://open/org/" + QString(506, 'a'), "/org/" + QString(506, 'a')},
        };
        for (const auto &[url, expected] : accepted) {
            const auto route = workspace::parseExternalDeepLink(url);
            QVERIFY2(route.has_value(), qPrintable(QStringLiteral("rejected: %1").arg(url)));
            QCOMPARE(*route, expected);
        }
    }

    void deepLinkRejectsEveryDocumentedEscapeAndShape()
    {
        const QString uuid = "66666666-6666-4666-8666-666666666666";
        const QStringList rejected = {
            QString(),
            QString("   "),
            QStringLiteral("ew://open/org/a\\b"),
            QStringLiteral("ew://open/cal endar"),
            QStringLiteral("ew://open/calendar\n"),
            QStringLiteral("ew://open/cal\rendar"),
            QStringLiteral("ew://open/cal\u0001endar"),
            QStringLiteral("https://open/calendar"),
            QStringLiteral("ew://evil/calendar"),
            QStringLiteral("ew://open:8443/calendar"),
            QStringLiteral("ew://user@open/calendar"),
            QStringLiteral("ew://user:pass@open/calendar"),
            QStringLiteral("ew://open//calendar"),
            QStringLiteral("ew://open/other"),
            QStringLiteral("ew://open/org"),
            QStringLiteral("ew://open/unknown/settings"),
            QStringLiteral("ew://open/calendar/extra"),
            QStringLiteral("ew://open/org/acme/unknown"),
            QStringLiteral("ew://open/org/acme/project/p1/unknown"),
            QStringLiteral("ew://open/org/acme/project/p1/stream/s/topic/t/deeper"),
            QStringLiteral("ew://open/org/acme/project/p1/message/not-a-uuid"),
            QStringLiteral("ew://open/org/acme/project/p1/message/"),
            QStringLiteral("ew://open/org/acme/project/p1/message/66666666-6666-4666-8666-66666666666"),
            QStringLiteral("ew://open/org/a%2Fb"),
            QStringLiteral("ew://open/org/a%2fb/settings"),
            QStringLiteral("ew://open/settings%5Cx"),
            QStringLiteral("ew://open/settings%00"),
            QStringLiteral("ew://open/settings%0A"),
            QStringLiteral("ew://open/settings%0d"),
            QStringLiteral("ew://open/org/acme/project/p1/message/") + uuid + QStringLiteral("?x=%2F"),
            QStringLiteral("ew://open/org/") + QString(508, 'a'),
            QStringLiteral("javascript:alert(1)"),
        };
        for (const auto &value : rejected)
            QVERIFY2(!workspace::parseExternalDeepLink(value).has_value(),
                qPrintable(QStringLiteral("accepted: %1").arg(value)));
    }

    void messageRouteExtractionReadsTheMessageTriple()
    {
        const QString uuid = "66666666-6666-4666-8666-666666666666";
        const QString project = "11111111-1111-4111-8111-111111111111";
        const auto found = workspace::parseMessageRoute("/org/acme/project/" + project + "/message/" + uuid + "?x=1");
        QVERIFY(found.has_value());
        QCOMPARE(found->org, QString("acme"));
        QCOMPARE(found->project, project);
        QCOMPARE(found->message, uuid);
        QVERIFY(!workspace::parseMessageRoute("/org/acme/project/" + project + "/stream/aaaaaaaa").has_value());
        QVERIFY(!workspace::parseMessageRoute("/calendar").has_value());
    }

    void organizationRouteIdMatchesTheSourceDerivation()
    {
        QCOMPARE(workspace::orgRouteIdFromOrigin(QUrl("https://app.example.com")), QString("app.example.com"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QUrl("https://App.Example.com:8443")),
                 QString("app.example.com-8443"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QUrl("http://localhost:3000")), QString("localhost-3000"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QUrl("http://localhost:80")), QString("localhost"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QUrl("https://127.0.0.1:8080")), QString("127.0.0.1-8080"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QString("example.com")), QString("example.com"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QString("Example.com")), QString("example.com"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QString("exa_mple.com")), QString("exa-mple.com"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QString("!!!")), QString("org"));
        QCOMPARE(workspace::orgRouteIdFromOrigin(QString("   ")), QString("org"));
    }

    void canonicalMessageUrlUsesTheSourceRouteShape()
    {
        const QString uuid = "66666666-6666-4666-8666-666666666666";
        const QString project = "11111111-1111-4111-8111-111111111111";
        QCOMPARE(workspace::encodeRouteSegment("abc-._~"), QString("abc-._~"));
        QCOMPARE(workspace::encodeRouteSegment("a*b"), QString("a%2Ab"));
        QCOMPARE(workspace::encodeRouteSegment("a b"), QString("a%20b"));
        QCOMPARE(workspace::encodeRouteSegment("100%"), QString("100%25"));
        QCOMPARE(workspace::encodeRouteSegment(QString::fromUtf8("ä")), QString("%C3%A4"));
        QCOMPARE(workspace::messageRoute("acme", project, uuid),
                 "/org/acme/project/" + project + "/message/" + uuid);
        QCOMPARE(workspace::messageUrl(QUrl("http://127.0.0.1:8123"), project, uuid),
                 QStringLiteral("http://127.0.0.1:8123/org/127.0.0.1-8123/project/%1/message/%2").arg(project, uuid));
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
