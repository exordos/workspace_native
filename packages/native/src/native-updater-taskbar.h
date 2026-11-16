#pragma once

#include <QObject>
#include <QPointer>
#include <QtGlobal>

class QWindow;

namespace workspace {

class NativeUpdater;

// Mirrors update download progress into the window's taskbar progress hint.
// See docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md for the measured
// reference contract (workspace_ui 61dacef7 packages/electron/src/main.ts:712-719,
// 1208-1211, 1363-1381). Qt 6.10 exposes no QWindow::setProgress, so the hint
// value (0..1, -1 cleared) is stored as the window's "progress" property at
// the single platform sink UpdaterTaskbarProgress::setHint().
class UpdaterTaskbarProgress final : public QObject {
    Q_OBJECT

public:
    explicit UpdaterTaskbarProgress(QObject *parent = nullptr);

    // C++ wiring only, never a QML invokable. Observes the updater until either
    // side is destroyed and mirrors the hint onto the window.
    void attach(QWindow *window, NativeUpdater *updater);

    // The reference IPC mapping: non-finite or negative becomes -1 (clear),
    // any other value clamps into [0, 1].
    static qreal hintFor(qreal progress);

    // Last hint pushed, -1 while nothing was pushed yet.
    qreal hint() const { return m_hint; }

private:
    void sync();
    void setHint(qreal hint);

    QPointer<QWindow> m_window;
    QPointer<NativeUpdater> m_updater;
    qreal m_hint = -1;
    bool m_hasHint = false;
};

} // namespace workspace
