#include "native-platform-shell.h"

#include <QImage>
#include <QPainter>
#include <QScopeGuard>
#include <QWindow>
#include <qt_windows.h>
#include <shobjidl.h>

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
}

void setPlatformUnread(QWindow *window, bool unread, bool, const QString &label)
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

void setPlatformTaskbarProgress(QWindow *window, qreal progress)
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

void setPlatformAttention(quintptr windowId, bool requested)
{
    if (!windowId) return;
    FLASHWINFO info{sizeof(FLASHWINFO), reinterpret_cast<HWND>(windowId),
                    DWORD(requested ? FLASHW_TRAY | FLASHW_TIMERNOFG : FLASHW_STOP), 0, 0};
    FlashWindowEx(&info);
}
}
