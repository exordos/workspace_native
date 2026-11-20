#include "native-desktop.h"
#include "native-platform-shell.h"

#include <QApplication>
#include <QPainter>
#include <QScopeGuard>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTest>
#include <QTranslator>
#include <QWindow>
#include <qt_windows.h>
#include <shellapi.h>
#include <algorithm>

namespace {
class OverlayTranslation final : public QTranslator {
public:
    mutable QString lastDescription;
    QString translate(const char *context, const char *source, const char *comment, int count) const override
    {
        const auto translated = QTranslator::translate(context, source, comment, count);
        if (qstrcmp(context, "workspace::NativeDesktop") == 0 && qstrcmp(source, "Unread messages") == 0)
            lastDescription = translated;
        return translated;
    }
};

// The window owns a distinctive icon so Explorer can finish inserting its exact
// taskbar button before we compare any progress or overlay pixels.
QIcon acceptanceIcon()
{
    QPixmap icon(32, 32);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.fillRect(6, 13, 20, 6, QColor(241, 0, 239));
    painter.fillRect(13, 6, 6, 20, QColor(241, 0, 239));
    painter.end();
    return QIcon(icon);
}

bool explorerHasTrayIcon()
{
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, nullptr, L"QTrayIconMessageWindow"))) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process != GetCurrentProcessId()) continue;
        NOTIFYICONIDENTIFIER id{};
        id.cbSize = sizeof(id);
        id.hWnd = window;
        // Qt registers icon ID zero. Explorer returns S_FALSE when that
        // registered icon is in the overflow area.
        RECT icon{};
        return SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon));
    }
    return false;
}

QImage paintedWindow(HWND window)
{
    auto *screen = QGuiApplication::primaryScreen();
    RECT rect{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!screen || !window || !IsWindowVisible(window) || !GetWindowRect(window, &rect)
        || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor))
        return {};
    // Explorer composes child surfaces separately. Read the visible desktop
    // rectangle, including the pixels its children actually painted.
    return screen->grabWindow(0).toImage().copy(
        rect.left - monitor.rcMonitor.left, rect.top - monitor.rcMonitor.top,
        rect.right - rect.left, rect.bottom - rect.top);
}

QImage taskbarPixels()
{
    const auto taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return {};
    auto *screen = QGuiApplication::primaryScreen();
    if (!screen) return {};
    // Only the app-button area belongs to the overlay. Capturing the
    // notification area would also see the separate dotted tray icon.
    HWND buttons = nullptr;
    EnumChildWindows(taskbar, [](HWND child, LPARAM data) -> BOOL {
        wchar_t name[64] = {};
        GetClassNameW(child, name, 64);
        if (wcscmp(name, L"MSTaskListWClass") != 0) return TRUE;
        *reinterpret_cast<HWND *>(data) = child;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&buttons));
    if (!buttons) return {};
    RECT buttonRect{}, taskbarRect{};
    if (!GetWindowRect(buttons, &buttonRect) || !GetWindowRect(taskbar, &taskbarRect)) return {};
    const auto taskbarImage = screen->grabWindow(reinterpret_cast<WId>(taskbar)).toImage();
    const auto image = taskbarImage.copy(buttonRect.left - taskbarRect.left, buttonRect.top - taskbarRect.top,
                             buttonRect.right - buttonRect.left, buttonRect.bottom - buttonRect.top)
        .convertToFormat(QImage::Format_RGB32);
    QRect mark;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto p = image.pixel(x, y);
            if (qRed(p) > 185 && qGreen(p) < 50 && qBlue(p) > 185)
                mark |= QRect(x, y, 1, 1);
        }
    if (mark.width() < 3 || mark.height() < 3) return {};
    // Retain the full height of our button, including a lower-corner overlay.
    const int half = image.height() / 2;
    return image.copy(QRect(mark.center().x() - half, 0, 2 * half, image.height()).intersected(image.rect()));
}

bool orange(QRgb pixel)
{
    return qRed(pixel) > 170 && qGreen(pixel) > 28 && qGreen(pixel) < 155 && qBlue(pixel) < 75;
}

bool green(QRgb pixel)
{
    return qGreen(pixel) > 65 && qGreen(pixel) > qRed(pixel) * 1.2 && qGreen(pixel) > qBlue(pixel) * 1.2;
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
    void progressReachesTheExplorerTaskbarAndClears()
    {
        QWindow window;
        window.setTitle("Workspace updater acceptance");
        window.resize(640, 400);
        window.setIcon(acceptanceIcon());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QImage before;
        QTRY_VERIFY_WITH_TIMEOUT(!(before = taskbarPixels()).isNull(), 10000);
        workspace::setPlatformTaskbarProgress(&window, .55);
        QVector<QPoint> filled;
        QTRY_VERIFY_WITH_TIMEOUT((filled = changedPixels(before, taskbarPixels(), green)).size() > 4, 10000);
        workspace::setPlatformTaskbarProgress(&window, -1);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto cleared = taskbarPixels();
            return cleared.size() == before.size() && std::none_of(filled.cbegin(), filled.cend(),
                [&](const QPoint &point) { return green(cleared.pixel(point)); });
        }(), 10000);
    }

    void explorerPaintsTheNormalAndUnreadNotificationIcon()
    {
        QWindow window;
        window.resize(640, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        workspace::NativeDesktop desktop(&window);
        QTRY_VERIFY(explorerHasTrayIcon());

        const auto taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        const auto area = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        QVERIFY(area && IsWindowVisible(area));
        const auto button = FindWindowExW(area, nullptr, L"Button", nullptr);
        HWND overflow = nullptr;
        if (button && IsWindowVisible(button)) {
            SendMessageW(button, BM_CLICK, 0, 0);
            QTRY_VERIFY((overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr))
                        && IsWindowVisible(overflow));
        }
        const auto close = qScopeGuard([&] {
            if (overflow && IsWindowVisible(overflow)) SendMessageW(button, BM_CLICK, 0, 0);
        });

        // Join only Explorer's notification areas. The separate unread
        // overlay on the app button must never satisfy this comparison.
        const auto pixels = [&] {
            const QImage visible = paintedWindow(area);
            const QImage hidden = paintedWindow(overflow);
            QImage result(visible.width() + hidden.width(), qMax(visible.height(), hidden.height()), QImage::Format_RGB32);
            result.fill(Qt::black);
            QPainter painter(&result);
            painter.drawImage(0, 0, visible);
            if (!hidden.isNull()) painter.drawImage(visible.width(), 0, hidden);
            painter.end();
            return result;
        };
        const auto normal = pixels();
        QVERIFY(!normal.isNull());
        desktop.setUnreadCount(7);
        QVector<QPoint> dots;
        QTRY_VERIFY_WITH_TIMEOUT(!(dots = changedPixels(normal, pixels(), orange)).isEmpty(), 10000);
        desktop.setUnreadCount(0);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto cleared = pixels();
            return cleared.size() == normal.size() && std::none_of(dots.cbegin(), dots.cend(),
                [&](const QPoint &p) { return orange(cleared.pixel(p)); });
        }(), 10000);
    }

    void unreadOverlayReachesTheExplorerTaskbar_data()
    {
        QTest::addColumn<bool>("russian");
        QTest::newRow("English") << false;
        QTest::newRow("Russian") << true;
    }

    void unreadOverlayReachesTheExplorerTaskbar()
    {
        QFETCH(bool, russian);
        OverlayTranslation translator;
        if (russian) {
            QVERIFY(translator.load(QStringLiteral("build/desktop-package/application/translations/workspace_ru.qm")));
            QVERIFY(QCoreApplication::installTranslator(&translator));
        }
        const auto remove = qScopeGuard([&] { if (russian) QCoreApplication::removeTranslator(&translator); });
        QWindow window;
        window.setTitle("Workspace desktop acceptance");
        window.resize(640, 400);
        window.setIcon(acceptanceIcon());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        workspace::NativeDesktop desktop(&window);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(explorerHasTrayIcon(), 5000);
        const auto normal = tray->icon().pixmap(32, 32).toImage();
        QVERIFY(!normal.isNull());
        QImage before;
        QTRY_VERIFY_WITH_TIMEOUT(!(before = taskbarPixels()).isNull(), 10000);
        translator.lastDescription.clear();
        desktop.setUnreadCount(7);
        if (russian) {
            QCOMPARE(translator.lastDescription, QStringLiteral("Непрочитанные сообщения"));
            qInfo().noquote() << "Unread accessibility label:" << translator.lastDescription;
        }
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

    }
};

QTEST_MAIN(WindowsShellTest)
#include "windows-taskbar.moc"
