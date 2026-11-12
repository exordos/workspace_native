#include "native-auth-guard.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
#include <QTimer>
#include <QWindow>

#include <algorithm>

namespace workspace {
namespace {
qint64 wallClockMs() { return QDateTime::currentMSecsSinceEpoch(); }

// auth-idle-timeout.lib.ts:10-26 AUTH_IDLE_TIMEOUT_MS_BY_PRESET.
constexpr struct { const char *preset; qint64 ms; } Presets[] = {
    {"6h", 6LL * 60 * 60 * 1000},
    {"12h", 12LL * 60 * 60 * 1000},
    {"24h", 24LL * 60 * 60 * 1000},
    {"3d", 3LL * 24 * 60 * 60 * 1000},
    {"7d", 7LL * 24 * 60 * 60 * 1000},
    {"never", -1},
};
} // namespace

NativeAuthGuard::NativeAuthGuard(QObject *parent, std::function<qint64()> nowMs)
    : QObject(parent), m_nowMs(nowMs ? std::move(nowMs) : wallClockMs)
{
    m_checkTimer.setSingleShot(true);
    connect(&m_checkTimer, &QTimer::timeout, this, [this] { schedule(); });
}

NativeAuthGuard::~NativeAuthGuard()
{
    removeEventFilter();
}

qint64 NativeAuthGuard::timeoutMsForPreset(const QString &preset)
{
    for (const auto &entry : Presets)
        if (preset == QLatin1String(entry.preset)) return entry.ms;
    return -1; // unknown values resolve through sanitizePreset upstream
}

QString NativeAuthGuard::sanitizePreset(const QString &value)
{
    for (const auto &entry : Presets)
        if (value == QLatin1String(entry.preset)) return QString::fromLatin1(entry.preset);
    return QStringLiteral("3d"); // settings.model.ts:75,94 default
}

void NativeAuthGuard::setNowProvider(std::function<qint64()> nowMs)
{
    if (!nowMs) return;
    m_nowMs = std::move(nowMs);
    schedule();
}

void NativeAuthGuard::setPollIntervalMs(int milliseconds)
{
    m_pollIntervalMs = std::max(1, milliseconds);
    schedule();
}

void NativeAuthGuard::setExpiryAction(std::function<void()> action)
{
    m_expiryAction = std::move(action);
}

void NativeAuthGuard::setTimeoutPreset(const QString &preset)
{
    // Reference layout-auth-guard.hook.ts: the effect cleanup tears the old
    // guard down exactly once, then re-initializes only if the new preset is
    // enabled.
    m_checkTimer.stop();
    m_fired = false;
    const bool wasActive = active();
    m_timeoutMs = timeoutMsForPreset(preset);
    m_deadlineMs = armed() ? nowMs() + m_timeoutMs : 0;
    if (armed())
        installEventFilter();
    else
        removeEventFilter();
    if (active() != wasActive)
        emit activeChanged();
    schedule();
}

void NativeAuthGuard::setSessionActive(bool active)
{
    if (m_sessionActive == active) return;
    const bool wasActive = this->active();
    m_sessionActive = active;
    m_fired = false;
    if (active)
    {
        if (m_timeoutMs > 0)
            m_deadlineMs = nowMs() + m_timeoutMs;
        installEventFilter();
    }
    else
    {
        m_deadlineMs = 0;
        m_checkTimer.stop();
        removeEventFilter();
    }
    if (this->active() != wasActive)
        emit activeChanged();
    schedule();
}

void NativeAuthGuard::setWindowsVisible(bool visible)
{
    m_visibilityExplicit = true;
    applyWindowsVisible(visible);
}

void NativeAuthGuard::applyWindowsVisible(bool visible)
{
    m_windowsVisible = visible;
    // auth-guard.ts:171-214: hidden clears the timer but keeps the stored
    // deadline; becoming visible reschedules from it and fires immediately if
    // the deadline already passed while hidden.
    if (visible)
        schedule();
    else
        m_checkTimer.stop();
}

void NativeAuthGuard::refreshWindowsVisibility()
{
    if (m_visibilityExplicit) return;
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return;
    bool anyVisible = false;
    for (const auto *window : QGuiApplication::topLevelWindows())
    {
        if (!window) continue;
        // PR #226 review (4555758286): the pause condition is "no window
        // visible/exposed"; a mapped window whose platform Expose has not
        // arrived (or a minimized X11 window) is visible but not exposed.
        if (window->isVisible()
            && window->isExposed()
            && window->visibility() != QWindow::Visibility::Hidden
            && window->visibility() != QWindow::Visibility::Minimized)
        {
            anyVisible = true;
            break;
        }
    }
    applyWindowsVisible(anyVisible);
}

void NativeAuthGuard::noteActivity()
{
    if (!armed()) return;
    // PR #226 review (4555758272/4555758307): the poll-based check timer can
    // lag the deadline, so activity may arrive after it already passed. The
    // expired deadline is enforced first; only a live deadline is re-armed.
    // Hidden windows pause the deadline, so expiry is enforced only while
    // windows are visible (matching the pause contract).
    if (m_windowsVisible && m_deadlineMs > 0 && nowMs() >= m_deadlineMs)
    {
        fire();
        return;
    }
    m_deadlineMs = nowMs() + m_timeoutMs;
    schedule();
}

bool NativeAuthGuard::armed() const
{
    return m_sessionActive && m_timeoutMs > 0 && !m_fired;
}

bool NativeAuthGuard::active() const
{
    return armed();
}

qint64 NativeAuthGuard::expiresAtMs() const
{
    return armed() ? m_deadlineMs : 0;
}

qint64 NativeAuthGuard::remainingMs() const
{
    if (!armed()) return 0;
    return m_deadlineMs - nowMs();
}

void NativeAuthGuard::schedule()
{
    m_checkTimer.stop();
    if (!armed()) return;
    if (!m_windowsVisible) return; // hidden: keep the deadline, clear the timer
    const qint64 remaining = m_deadlineMs - nowMs();
    if (remaining <= 0)
    {
        fire();
        return;
    }
    m_checkTimer.start(static_cast<int>(std::min<qint64>(remaining, m_pollIntervalMs)));
}

void NativeAuthGuard::fire()
{
    m_checkTimer.stop();
    m_fired = true;
    removeEventFilter();
    emit activeChanged();
    // auth-guard.ts:132-169: the sign-out runs even if a hook fails; native
    // rethrows here would abort unrelated application code instead.
    if (m_expiryAction)
    {
        try { m_expiryAction(); } catch (...) {}
    }
}

void NativeAuthGuard::installEventFilter()
{
    if (m_filterInstalled || !QCoreApplication::instance()) return;
    qApp->installEventFilter(this);
    m_filterInstalled = true;
    refreshWindowsVisibility();
}

void NativeAuthGuard::removeEventFilter()
{
    if (!m_filterInstalled) return;
    if (QCoreApplication::instance())
        qApp->removeEventFilter(this);
    m_filterInstalled = false;
}

qint64 NativeAuthGuard::nowMs() const
{
    return m_nowMs ? m_nowMs() : wallClockMs();
}

bool NativeAuthGuard::eventFilter(QObject *, QEvent *event)
{
    switch (event->type())
    {
    // Qt counterparts of auth-guard.ts:239 mousemove/keydown/click/scroll.
    case QEvent::MouseMove:
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
        noteActivity();
        return false;
    // Window visibility changes are the document.hidden analogue, not activity.
    case QEvent::Expose:
    case QEvent::WindowStateChange:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::PlatformSurface:
        refreshWindowsVisibility();
        return false;
    default:
        return false;
    }
}

} // namespace workspace
