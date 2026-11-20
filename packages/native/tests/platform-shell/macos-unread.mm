#import <AppKit/AppKit.h>
#include "native-desktop.h"

#include <QApplication>
#include <QFile>
#include <QSystemTrayIcon>
#include <QTest>
#include <QWindow>

namespace {
QByteArray pixels(NSImage *image)
{
    if (!image) return {};
    NSRect rect = NSMakeRect(0, 0, 128, 128);
    CGImageRef source = [image CGImageForProposedRect:&rect context:nil hints:nil];
    if (!source) return {};
    QByteArray rgba(128 * 128 * 4, 0);
    auto color = CGColorSpaceCreateDeviceRGB();
    auto context = CGBitmapContextCreate(rgba.data(), 128, 128, 8, 128 * 4,
                                         color, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(color);
    if (!context) return {};
    CGContextDrawImage(context, CGRectMake(0, 0, 128, 128), source);
    CGContextRelease(context);
    return rgba;
}

QByteArray expectedDockIcon(const QString &file)
{
    return pixels([[NSImage alloc] initWithContentsOfFile:file.toNSString()]);
}
}

class MacShellTest final : public QObject {
    Q_OBJECT
private slots:
    void unreadAndCallStateReachTheRealDock()
    {
        const auto normal = expectedDockIcon(QFINDTESTDATA("../../../electron/resources/dock-icon.png"));
        const auto unread = expectedDockIcon(QFINDTESTDATA("../../../electron/resources/dock-icon-unread.png"));
        QVERIFY(!normal.isEmpty() && !unread.isEmpty() && normal != unread);
        QWindow window;
        window.resize(640, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        workspace::NativeDesktop desktop(&window);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->isVisible());
        auto trayPixels = [tray] { return tray->icon().pixmap(32, 32).toImage(); };
        const auto normalTray = trayPixels();
        QVERIFY(!normalTray.isNull());
        desktop.setUnreadCount(7);
        QTRY_VERIFY(pixels(NSApp.applicationIconImage) == unread);
        QVERIFY(NSApp.dockTile.badgeLabel.length == 0);
        QVERIFY(trayPixels() != normalTray);
        desktop.setCallState("test room", 2);
        QTRY_VERIFY([NSApp.dockTile.badgeLabel isEqualToString:@"📞"]);
        QVERIFY(pixels(NSApp.applicationIconImage) == normal);
        desktop.setUnreadCount(8);
        QVERIFY([NSApp.dockTile.badgeLabel isEqualToString:@"📞"]);
        desktop.setCallState({}, -1);
        QTRY_VERIFY(pixels(NSApp.applicationIconImage) == unread);
        QVERIFY(NSApp.dockTile.badgeLabel.length == 0);
        desktop.setUnreadCount(0);
        QTRY_VERIFY(pixels(NSApp.applicationIconImage) == normal);
        QCOMPARE(trayPixels(), normalTray);
    }
};

QTEST_MAIN(MacShellTest)
#include "macos-unread.moc"
