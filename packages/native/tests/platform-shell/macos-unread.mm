#import <AppKit/AppKit.h>
#include "native-desktop.h"

#include <QApplication>
#include <QTest>
#include <QWindow>

class MacShellTest final : public QObject {
    Q_OBJECT
private slots:
    void unreadBadgeFollowsTheDesktopState()
    {
        QWindow window;
        window.resize(640, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        workspace::NativeDesktop desktop(&window);
        desktop.setUnreadCount(7);
        QTRY_VERIFY([NSApp.dockTile.badgeLabel isEqualToString:@"7"]);
        desktop.setUnreadCount(0);
        QTRY_VERIFY(NSApp.dockTile.badgeLabel.length == 0);
    }
};

QTEST_MAIN(MacShellTest)
#include "macos-unread.moc"
