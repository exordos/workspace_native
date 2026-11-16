#include "native-updater-taskbar.h"

#include "native-updater.h"

#include <QVariant>
#include <QWindow>

#include <cmath>

namespace workspace {

namespace {
// The QWindow progress hint property. Qt 6.10 exposes no QWindow::setProgress
// yet (docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md), so the hint lives as
// this window property; bare window managers and this host's bare Xvnc dock
// ignore any taskbar hint regardless.
const char *const kWindowProgressProperty = "progress";
}

UpdaterTaskbarProgress::UpdaterTaskbarProgress(QObject *parent)
    : QObject(parent)
{
}

qreal UpdaterTaskbarProgress::hintFor(qreal progress)
{
    // main.ts:1210 maps a non-finite value to the clear value; main.ts:712-719
    // maps negative to clear and clamps everything else into [0, 1].
    if (!std::isfinite(progress) || progress < 0.0) return -1.0;
    return qMin(progress, qreal(1.0));
}

void UpdaterTaskbarProgress::attach(QWindow *window, NativeUpdater *updater)
{
    m_window = window;
    m_updater = updater;
    if (!updater) return;
    connect(updater, &NativeUpdater::changed, this, &UpdaterTaskbarProgress::sync);
}

void UpdaterTaskbarProgress::sync()
{
    if (!m_window || !m_updater) return;
    const auto state = m_updater->state();
    if (state == QStringLiteral("downloading")) {
        // download-progress pushes percent/100 (main.ts:1363-1371); the native
        // progress property is already that fraction.
        setHint(hintFor(m_updater->progress()));
    } else if (state == QStringLiteral("downloaded") || state == QStringLiteral("error")
        || state == QStringLiteral("idle")) {
        // update-downloaded and error clear the bar (main.ts:1373-1381); the
        // native cancel() lands in idle and must not leave a stuck bar.
        setHint(-1.0);
    }
    // checking/available/current/installing/ready: the reference pushes no
    // progress on these events, so the previous hint stays untouched.
}

void UpdaterTaskbarProgress::setHint(qreal hint)
{
    if (m_hasHint && hint == m_hint) return; // QWindow::setProgress dedupe.
    m_hint = hint;
    m_hasHint = true;
    m_window->setProperty(kWindowProgressProperty, hint);
}

} // namespace workspace
