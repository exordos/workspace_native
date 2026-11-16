#include "native-updater-taskbar.h"

#include "native-updater.h"

#include <QVariant>
#include <QWindow>

#include <cmath>

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#endif

namespace workspace {

namespace {
// The internal mirror of the last hint. Qt 6.10 exposes no QWindow::setProgress
// (docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md), so the value is kept as
// this window property for the Qt API to adopt later; it is not itself a
// platform channel - the LauncherEntry publish below is.
const char *const kWindowProgressProperty = "progress";

#ifdef Q_OS_LINUX
// LauncherEntry taskbar protocol, the desktop standard Unity7, GNOME
// Dash-to-Dock, Plank, Latte and (consumer-side) KDE Plasma docks honour for
// taskbar progress:
//   signal  com.canonical.Unity.LauncherEntry.Update(s app-uri, a{sv} properties)
//   app-uri application://<desktop-file-basename>.desktop
//   properties  progress-visible (b), progress (d, 0..1);
//               also count (x), count-visible (b), urgent (b)
// The application sends no method call and owns nothing: it broadcasts the
// Update signal on the session bus and every launcher listening through a
// sender-agnostic signal match rule receives it (GNOME extensions do
// `signal_subscribe(null, 'com.canonical.Unity.LauncherEntry', 'Update', null, ...)`;
// libunity likewise broadcasts from an arbitrary per-app path, and "the exact
// object path is not important"). With no launcher listening the daemon has no
// receiver and the broadcast is simply dropped - harmless on bare WMs.
// Citations (fetched 2026-11-16): docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md
// ("Platform channel" section quotes the wiki.ubuntu.com/Unity/LauncherAPI
// low-level D-Bus API and the snapcraft/libunity path notes).
const QString LauncherEntryInterface = QStringLiteral("com.canonical.Unity.LauncherEntry");
// Any path is spec-valid; consumers match interface + member, not path.
const QString LauncherEntryPath = QStringLiteral("/com/canonical/Unity/LauncherEntry");
// app-uri is "application://" + the basename of the installed .desktop file;
// this application's file is resources/workspace-native.desktop, installed as
// com.exordos.workspace.native.preview.desktop (packages/native/CMakeLists.txt,
// "install(FILES resources/workspace-native.desktop ... RENAME ...)").
const QString LauncherEntryAppUri =
    QStringLiteral("application://com.exordos.workspace.native.preview.desktop");
#endif
} // namespace

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
    publish(hint);
}

void UpdaterTaskbarProgress::publish(qreal hint)
{
#ifdef Q_OS_LINUX
    if (!QDBusConnection::sessionBus().isConnected()) return; // headless: nothing to publish to
    // Broadcast (no destination) per the protocol: real launchers subscribe
    // with sender-agnostic match rules, so a broadcast covers the name owner
    // and every match-rule listener at once. With no listener the daemon drops
    // it - fire-and-forget with no delivery state to track.
    QDBusMessage update = QDBusMessage::createSignal(LauncherEntryPath, LauncherEntryInterface,
        QStringLiteral("Update"));
    update << LauncherEntryAppUri;
    QVariantMap properties;
    properties.insert(QStringLiteral("progress-visible"), hint >= 0.0);
    if (hint >= 0.0) properties.insert(QStringLiteral("progress"), hint);
    update << properties;
    // Fire-and-forget: without a listening launcher the daemon simply drops the
    // signal (harmless on bare WMs), so no delivery state exists to track.
    QDBusConnection::sessionBus().send(update);
#else
    Q_UNUSED(hint); // macOS DockTile / Windows ITaskbarList3: out of scope (see docs).
#endif
}

} // namespace workspace
