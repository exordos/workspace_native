#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QtGlobal>

class QWindow;
class QTimer;

namespace workspace {

class NativeUpdater;

// Mirrors update download progress into the OS taskbar progress hint.
// See docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md for the measured
// reference contract (workspace_ui 61dacef7 packages/electron/src/main.ts:712-719,
// 1208-1211, 1363-1381) and for the platform channel design.
//
// setHint() is the single platform sink: it stores the hint value (0..1, -1
// cleared) as the window's "progress" property AND publishes it to the real
// desktop channel. On Linux that channel is the com.canonical.Unity.LauncherEntry
// Update signal (Unity7, GNOME Dash-to-Dock, Plank, Latte dock): the
// documented wire protocol taskbar progress actually travels on, since EWMH
// defines no progress hint and Qt 6.10 exposes no QWindow::setProgress.
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
    void publish(qreal hint);
    // Throttled republish of the current progress hint (Linux only, no-op
    // elsewhere): a consumer that only matches the Update signal (no well-known
    // name) that appears while a download has stalled cannot be woken by
    // NameOwnerChanged, so while a progress hint is active it is re-broadcast on
    // a fixed interval; the timer stops with the hint.
    void startRepublishing();
    void stopRepublishing();

private slots:
    // Session-bus org.freedesktop.DBus.NameOwnerChanged sink: a LauncherEntry
    // name (re)appearing means a launcher that missed the earlier broadcasts
    // just started; republish the current hint to it (Linux only, no-op
    // elsewhere).
    void onLauncherNameOwnerChanged(const QString &name, const QString &oldOwner,
        const QString &newOwner);

private:
    QPointer<QWindow> m_window;
    QPointer<NativeUpdater> m_updater;
    QTimer *m_republishTimer = nullptr;
    qreal m_hint = -1;
    bool m_hasHint = false;
};

} // namespace workspace
