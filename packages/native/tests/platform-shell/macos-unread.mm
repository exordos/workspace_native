#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include "native-desktop.h"
#include "native-notifications.h"
#include "native-platform-shell.h"

#include <QApplication>
#include <QScopeGuard>
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

NSStatusItem *statusItem = nil;
using CreateStatusItem = NSStatusItem *(*)(id, SEL, CGFloat);
CreateStatusItem originalCreateItem = nullptr;
NSStatusItem *observedStatusItem(id bar, SEL selector, CGFloat length)
{
    statusItem = originalCreateItem(bar, selector, length);
    return statusItem;
}

int attentionRequests = 0;
NSRequestUserAttentionType lastAttentionType{};
using RequestAttention = NSInteger (*)(id, SEL, NSRequestUserAttentionType);
RequestAttention originalRequest = nullptr;
NSInteger observedRequest(id app, SEL selector, NSRequestUserAttentionType type)
{
    ++attentionRequests;
    lastAttentionType = type;
    return originalRequest(app, selector, type);
}

QByteArray expectedDockIcon(const QString &file)
{
    return pixels([[NSImage alloc] initWithContentsOfFile:file.toNSString()]);
}
}

class MacShellTest final : public QObject {
    Q_OBJECT
private slots:
    void progressAppearsOnTheDockTileAndClears()
    {
        const auto previous = NSApp.dockTile.contentView;
        workspace::setPlatformTaskbarProgress(nullptr, .25);
        NSProgressIndicator *bar = nil;
        for (NSView *child in NSApp.dockTile.contentView.subviews) {
            if ([child isKindOfClass:NSProgressIndicator.class])
                bar = (NSProgressIndicator *)child;
        }
        QVERIFY(bar);
        QCOMPARE(bar.doubleValue, .25);
        workspace::setPlatformTaskbarProgress(nullptr, .75);
        QCOMPARE(bar.doubleValue, .75);
        workspace::setPlatformTaskbarProgress(nullptr, -1);
        QCOMPARE(NSApp.dockTile.contentView, previous);
    }

    void backgroundAttentionRequestsAnInformationalDockBounce()
    {
        const auto method = class_getInstanceMethod(NSApp.class, @selector(requestUserAttention:));
        QVERIFY(method);
        originalRequest = reinterpret_cast<RequestAttention>(
            method_setImplementation(method, reinterpret_cast<IMP>(observedRequest)));
        const auto restore = qScopeGuard([method] {
            method_setImplementation(method, reinterpret_cast<IMP>(originalRequest));
        });
        const int before = attentionRequests;
        QWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        workspace::DBusNotificationTransport notifications([&window] { return quintptr(window.winId()); });
        notifications.requestAttention();
        QCOMPARE(attentionRequests, before + 1);
        QCOMPARE(lastAttentionType, NSInformationalRequest);
        notifications.clearAttention();
    }

    void unreadAndCallStateReachTheRealDock()
    {
        const auto normal = expectedDockIcon(QFINDTESTDATA("../../../electron/resources/dock-icon.png"));
        const auto unread = expectedDockIcon(QFINDTESTDATA("../../../electron/resources/dock-icon-unread.png"));
        QVERIFY(!normal.isEmpty() && !unread.isEmpty() && normal != unread);
        QWindow window;
        window.resize(640, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto createItem = class_getInstanceMethod(NSStatusBar.systemStatusBar.class, @selector(statusItemWithLength:));
        QVERIFY(createItem);
        originalCreateItem = reinterpret_cast<CreateStatusItem>(
            method_setImplementation(createItem, reinterpret_cast<IMP>(observedStatusItem)));
        const auto restore = qScopeGuard([createItem] {
            method_setImplementation(createItem, reinterpret_cast<IMP>(originalCreateItem));
            statusItem = nil;
        });
        workspace::NativeDesktop desktop(&window);
        QTRY_VERIFY(statusItem.button.image);
        const auto nativeTrayNormal = pixels(statusItem.button.image);
        QVERIFY(!nativeTrayNormal.isEmpty());
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->isVisible());
        auto trayPixels = [tray] { return tray->icon().pixmap(32, 32).toImage(); };
        const auto normalTray = trayPixels();
        QVERIFY(!normalTray.isNull());
        desktop.setUnreadCount(7);
        QTRY_VERIFY(pixels(NSApp.applicationIconImage) == unread);
        QVERIFY(NSApp.dockTile.badgeLabel.length == 0);
        QVERIFY(trayPixels() != normalTray);
        QTRY_VERIFY(pixels(statusItem.button.image) != nativeTrayNormal);
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
        QTRY_VERIFY(pixels(statusItem.button.image) == nativeTrayNormal);
    }
};

QTEST_MAIN(MacShellTest)
#include "macos-unread.moc"
