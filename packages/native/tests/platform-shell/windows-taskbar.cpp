#include "native-desktop.h"

#include <QScreen>
#include <QSystemTrayIcon>
#include <QTest>
#include <QTranslator>
#include <QWindow>
#include <qt_windows.h>
#include <algorithm>

namespace {
QImage taskbarPixels()
{
    const auto taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return {};
    auto *screen = QGuiApplication::primaryScreen();
    if (!screen) return {};
    return screen->grabWindow(reinterpret_cast<WId>(taskbar)).toImage().convertToFormat(QImage::Format_RGB32);
}

bool orange(QRgb pixel)
{
    return qRed(pixel) > 170 && qGreen(pixel) > 28 && qGreen(pixel) < 155 && qBlue(pixel) < 75;
}

template<class Color>
QVector<QPoint> changedPixels(const QImage &before, const QImage &after, Color matches)
{
    QVector<QPoint> dots;
    if (before.size() != after.size()) return dots;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            if (!matches(before.pixel(x, y)) && matches(after.pixel(x, y)))
                dots.append({x, y});
    return dots;
}
}

class WindowsShellTest final : public QObject {
    Q_OBJECT
private slots:
    void unreadOverlayReachesTheExplorerTaskbar()
    {
        QWindow window;
        window.setTitle("Workspace desktop acceptance");
        window.resize(640, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        workspace::NativeDesktop desktop(&window);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->isVisible());
        const auto normal = tray->icon().pixmap(32, 32).toImage();
        QVERIFY(!normal.isNull());
        const auto before = taskbarPixels();
        QVERIFY2(!before.isNull(), "The smoke runner requires an interactive Explorer taskbar.");
        desktop.setUnreadCount(7);
        QVERIFY(tray->icon().pixmap(32, 32).toImage() != normal);
        QVector<QPoint> dots;
        QTRY_VERIFY_WITH_TIMEOUT(!(dots = changedPixels(before, taskbarPixels(), orange)).isEmpty(), 10000);
        desktop.setUnreadCount(0);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto cleared = taskbarPixels();
            return cleared.size() == before.size() && std::none_of(dots.cbegin(), dots.cend(),
                [&](const QPoint &point) { return orange(cleared.pixel(point)); });
        }(), 10000);
        QCOMPARE(tray->icon().pixmap(32, 32).toImage(), normal);

        QTranslator russian;
        QVERIFY(russian.load(QStringLiteral("build/desktop-package/application/translations/workspace_ru.qm")));
        QCOMPARE(russian.translate("workspace::NativeDesktop", "Unread messages"), QStringLiteral("Непрочитанные сообщения"));
    }
};

QTEST_MAIN(WindowsShellTest)
#include "windows-taskbar.moc"
