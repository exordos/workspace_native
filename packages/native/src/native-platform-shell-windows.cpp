#include "native-platform-shell.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QScopeGuard>
#include <QWindow>
#include <qt_windows.h>
#include <shobjidl.h>
#include <optional>

namespace workspace {
namespace {
template<class Operation>
void withTaskbar(QWindow *window, Operation operation)
{
    if (!window || !window->handle()) return;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return;
    const auto uninitialize = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized)) CoUninitialize();
    });
    ITaskbarList3 *taskbar = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar)))) return;
    const auto release = qScopeGuard([taskbar] { taskbar->Release(); });
    if (SUCCEEDED(taskbar->HrInit()))
        operation(taskbar, reinterpret_cast<HWND>(window->winId()));
}

void publishUnread(QWindow *window, bool unread, const QString &label)
{
    withTaskbar(window, [&](ITaskbarList3 *taskbar, HWND handle) {
        HICON icon = nullptr;
        if (unread) {
            QImage dot(16, 16, QImage::Format_ARGB32_Premultiplied);
            dot.fill(Qt::transparent);
            QPainter painter(&dot);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 85, 0));
            painter.drawEllipse(QPointF(14, 2), 2, 2);
            painter.end();
            icon = dot.toHICON();
        }
        taskbar->SetOverlayIcon(handle, icon, unread ? reinterpret_cast<LPCWSTR>(label.utf16()) : nullptr);
        if (icon) DestroyIcon(icon);
    });
}

void publishProgress(QWindow *window, qreal progress)
{
    withTaskbar(window, [progress](ITaskbarList3 *taskbar, HWND handle) {
        if (progress < 0) {
            taskbar->SetProgressState(handle, TBPF_NOPROGRESS);
            return;
        }
        taskbar->SetProgressState(handle, TBPF_NORMAL);
        taskbar->SetProgressValue(handle, qRound64(progress * 1000), 1000);
    });
}

// Explorer sends TaskbarButtonCreated for a new window button and again
// after the shell restarts. Keep only the latest hints belonging to this window.
class WindowTaskbarHints final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit WindowTaskbarHints(QWindow *window)
        : QObject(window), m_window(window)
    {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }

    static WindowTaskbarHints *forWindow(QWindow *window)
    {
        if (!window) return nullptr;
        auto *hints = window->findChild<WindowTaskbarHints *>(Qt::FindDirectChildrenOnly);
        return hints ? hints : new WindowTaskbarHints(window);
    }

    void setUnread(bool unread, const QString &label)
    {
        m_unread = unread;
        m_label = label;
        publishUnread(m_window, unread, label);
    }

    void setProgress(qreal progress)
    {
        m_progress = progress;
        publishProgress(m_window, progress);
    }

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
    {
        if (type != "windows_generic_MSG") return false;
        const auto *event = static_cast<const MSG *>(message);
        if (m_buttonCreated && event->message == m_buttonCreated && m_window->handle()
            && event->hwnd == reinterpret_cast<HWND>(m_window->winId())) {
            if (m_unread.has_value()) publishUnread(m_window, *m_unread, m_label);
            if (m_progress.has_value()) publishProgress(m_window, *m_progress);
        }
        return false;
    }

private:
    QWindow *m_window;
    const UINT m_buttonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    std::optional<bool> m_unread;
    QString m_label;
    std::optional<qreal> m_progress;
};
}

void setPlatformUnread(QWindow *window, bool unread, bool, const QString &label)
{
    if (auto *hints = WindowTaskbarHints::forWindow(window)) hints->setUnread(unread, label);
}

void setPlatformTaskbarProgress(QWindow *window, qreal progress)
{
    if (auto *hints = WindowTaskbarHints::forWindow(window)) hints->setProgress(progress);
}

void setPlatformAttention(quintptr windowId, bool requested)
{
    if (!windowId) return;
    FLASHWINFO info{sizeof(FLASHWINFO), reinterpret_cast<HWND>(windowId),
                    DWORD(requested ? FLASHW_TRAY | FLASHW_TIMERNOFG : FLASHW_STOP), 0, 0};
    FlashWindowEx(&info);
}
}

#include "native-platform-shell-windows.moc"
