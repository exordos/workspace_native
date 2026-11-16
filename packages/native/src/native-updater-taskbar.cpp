#include "native-updater-taskbar.h"

#include "native-updater.h"

#include <QVariant>
#include <QWindow>

#include <cmath>

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QTimer>
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
// Republish interval of the throttled heartbeat (defended in
// docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md): a consumer that only
// matches the Update signal and appears while a download is stalled must see
// the current hint within a few seconds, while the bus only ever carries one
// small frame per interval (0.5 signals/s worst case, and only while a
// progress hint is active).
constexpr int kRepublishIntervalMs = 2000;
#endif
} // namespace

UpdaterTaskbarProgress::UpdaterTaskbarProgress(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_LINUX
    // The Update broadcast is not replayed: a launcher that starts - or
    // restarts - while a download has stalled missed every earlier frame and
    // receives nothing until new progress arrives. Watch for the LauncherEntry
    // name (re)appearing on the session bus and republish the current hint
    // then (reviewer finding 4581477136).
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("NameOwnerChanged"), this,
        SLOT(onLauncherNameOwnerChanged(QString, QString, QString)));
#endif
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
    // A progress hint must stay reachable to consumers that appear late on
    // match rules only (they own no bus name, so NameOwnerChanged cannot wake
    // the mirror); run the throttled republish until the hint clears. A
    // cleared hint (-1: downloaded/error/cancel/idle) ends the republish -
    // a clear frame needs no repetition because a late consumer's default
    // state already shows no bar.
    if (hint >= 0.0) startRepublishing();
    else stopRepublishing();
}

void UpdaterTaskbarProgress::startRepublishing()
{
#ifdef Q_OS_LINUX
    if (!m_republishTimer) {
        m_republishTimer = new QTimer(this);
        m_republishTimer->setInterval(kRepublishIntervalMs);
        // Bypass the setHint() dedupe on purpose: the wire value may be
        // unchanged, but a consumer that only just subscribed has not
        // received it. publish() is silent without a session bus.
        connect(m_republishTimer, &QTimer::timeout, this, [this] { publish(m_hint); });
    }
    if (!m_republishTimer->isActive()) m_republishTimer->start();
#endif
}

void UpdaterTaskbarProgress::stopRepublishing()
{
#ifdef Q_OS_LINUX
    if (m_republishTimer) m_republishTimer->stop();
#endif
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

void UpdaterTaskbarProgress::onLauncherNameOwnerChanged(const QString &name,
    const QString &oldOwner, const QString &newOwner)
{
    Q_UNUSED(oldOwner); // A lost-then-gained restart is two signals; the gain is the one that matters.
#ifdef Q_OS_LINUX
    // Non-empty new owner: the LauncherEntry side of the protocol just became
    // the bus name its listeners address - a launcher that appeared or
    // restarted and therefore never saw the frames already published. Call
    // publish() directly instead of setHint() so the dedupe on m_hint cannot
    // swallow the frame: the wire value is unchanged, but this listener has
    // not received it. With no hint pushed yet there is nothing to say, so
    // stay silent exactly as before the first updater state.
    if (name != LauncherEntryInterface || newOwner.isEmpty() || !m_hasHint) return;
    publish(m_hint); // progress-visible + progress, or the clear state.
#else
    Q_UNUSED(name);
    Q_UNUSED(newOwner);
#endif
}

} // namespace workspace
