#include "call/native-call-window.h"

#include "call/jitsi-call.h"

#include <QUrl>

namespace workspace {

namespace {
QString trimmed(const QVariantMap &call, const QString &key)
{
    return call.value(key).toString().trimmed();
}

// The port a URL actually connects to: its explicit port, or its own
// scheme's default (https -> 443, http -> 80). Evaluated per URL,
// independently of any other URL, so a default-port frame can never inherit
// the grants of a meeting hosted on a non-default port (review finding,
// exo-review thread 4559430270).
int effectivePort(const QUrl &url)
{
    const int explicitPort = url.port(-1);
    if (explicitPort != -1)
        return explicitPort;
    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0)
        return 443;
    if (url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0)
        return 80;
    return -1; // unknown scheme with no port: only matches another portless URL
}
} // namespace

QString JitsiCallWindowController::allowPolicy()
{
    // jitsi-call-permissions.lib.ts:4 — applied once when the embed surface
    // is created; shell toggles must not look like a new session.
    return QStringLiteral("camera; microphone; fullscreen; display-capture");
}

JitsiCallWindowController::JitsiCallWindowController(QObject *parent) : QObject(parent)
{
    // Focus-aware reconcile poll (jitsi-participant-count.hook.ts: start on
    // apiReady, restart on focus change, clear on close): the tick re-reads
    // the injected count provider exactly like a manual pollNow.
    m_pollTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &JitsiCallWindowController::pollNow);
}

QRect JitsiCallWindowController::expandedBounds(const QSize &viewport)
{
    // jitsi-call-modal-shell.hook.ts getExpandedWindowBounds.
    const auto inset = viewport.width() >= kDesktopBreakpoint
            && viewport.height() >= kDesktopBreakpoint
        ? kDesktopInset : kMobileInset;
    return QRect(inset, inset,
        qMax(kMinWindowWidth, viewport.width() - inset * 2),
        qMax(kMinWindowHeight, viewport.height() - inset * 2));
}

QRect JitsiCallWindowController::defaultPipBounds(const QSize &viewport)
{
    // jitsi-call-pip.lib.ts getDefaultPipWindowBounds: bottom-right anchor
    // with a 20 px margin, floored at the margin for tiny viewports.
    return QRect(qMax(kPipMargin, viewport.width() - kDefaultPipWidth - kPipMargin),
        qMax(kPipMargin, viewport.height() - kDefaultPipHeight - kPipMargin),
        kDefaultPipWidth, kDefaultPipHeight);
}

QRect JitsiCallWindowController::defaultPipBounds(const QSize &viewport, const QPoint &screenOrigin)
{
    // The viewport-relative default mapped through the hosting window's
    // global position: the tool window needs screen coordinates (finding 6).
    QRect bounds = defaultPipBounds(viewport);
    bounds.translate(screenOrigin);
    return bounds;
}

QRect JitsiCallWindowController::bounds() const
{
    return m_mode == Minimized ? m_pipBounds : expandedBounds(m_viewport);
}

int JitsiCallWindowController::pollIntervalMs() const
{
    if (m_pollIntervalOverrideMs > 0)
        return m_pollIntervalOverrideMs;
    return m_focused ? kParticipantsPollMs : kParticipantsPollUnfocusedMs;
}

void JitsiCallWindowController::setViewportSize(const QSize &viewport)
{
    if (m_viewport == viewport)
        return;
    m_viewport = viewport;
    // Only the expanded window follows the viewport; PiP bounds are user space.
    if (m_mode != Minimized)
        emit boundsChanged();
}

void JitsiCallWindowController::setFocused(bool focused)
{
    m_focused = focused;
    // Focus changes the cadence mid-call (5000 ms focused, 30000 ms not).
    if (m_pollTimer.isActive())
        m_pollTimer.setInterval(pollIntervalMs());
}

void JitsiCallWindowController::setPipBounds(const QRect &bounds)
{
    // The PiP surface stays resizable down to the shared 280x180 minimum.
    // Anything the user places is screen space from now on (finding 6).
    m_pipBoundsFromUser = true;
    m_pipBounds = QRect(bounds.x(), bounds.y(),
        qMax(kMinWindowWidth, bounds.width()), qMax(kMinWindowHeight, bounds.height()));
    if (m_mode == Minimized)
        emit boundsChanged();
}

void JitsiCallWindowController::open(const QVariantMap &activeCall)
{
    m_call = activeCall;
    const auto parsed = parseJitsiUrl(trimmed(m_call, QStringLiteral("meetingUrl")),
        m_call.value(QStringLiteral("meetUrl")).toString());
    m_domain = parsed.value(QStringLiteral("domain")).toString();
    m_roomName = parsed.value(QStringLiteral("roomName")).toString();
    // New session: reset per-call state (the reference resets pip bounds,
    // fullscreen, minimized and the participant count on open).
    m_pipBounds = defaultPipBounds(m_viewport);
    m_pipBoundsFromUser = false;
    m_fullscreen = false;
    m_permissionsGranted = false;
    m_participantCount = -1;
    m_embedState = m_roomName.isEmpty() ? InvalidUrl : Loading;
    m_pollTimer.stop();
    m_mode = Expanded;
    emit modeChanged();
    emit boundsChanged();
    emit embedStateChanged();
    emit fullscreenChanged();
    emit participantCountChanged();
    emit textsChanged();
}

void JitsiCallWindowController::close()
{
    if (m_mode == Closed)
        return;
    m_mode = Closed;
    m_embedState = Idle;
    m_permissionsGranted = false;
    m_participantCount = -1;
    m_fullscreen = false;
    m_pollTimer.stop();
    emit modeChanged();
    emit boundsChanged();
    emit embedStateChanged();
    emit permissionsChanged();
    emit fullscreenChanged();
    emit participantCountChanged();
    emit textsChanged();
    emit closeRequested();
}

void JitsiCallWindowController::toggleMinimize()
{
    if (m_mode == Closed)
        return;
    if (m_mode == Expanded) {
        // Minimizing exits browser fullscreen first (reference toggleMinimized).
        if (m_fullscreen) {
            m_fullscreen = false;
            emit fullscreenChanged();
        }
        m_mode = Minimized;
    } else {
        m_mode = Expanded;
    }
    emit modeChanged();
    emit boundsChanged();
    emit textsChanged();
}

void JitsiCallWindowController::toggleFullscreen()
{
    // The fullscreen control only exists while expanded.
    if (m_mode != Expanded)
        return;
    m_fullscreen = !m_fullscreen;
    emit fullscreenChanged();
    emit textsChanged();
}

void JitsiCallWindowController::requestDismiss()
{
    // Outside click / Escape never dismiss a live call: while expanded it
    // minimizes, while minimized it is ignored entirely.
    if (m_mode == Expanded)
        toggleMinimize();
}

void JitsiCallWindowController::readyToClose()
{
    close();
}

void JitsiCallWindowController::apiLoadSucceeded()
{
    if (m_mode == Closed || m_embedState != Loading)
        return;
    m_embedState = Ready;
    emit embedStateChanged();
    ensureEmbed();
    syncParticipantCount();
    // Reference handleApiReady: reconcile poll runs for the whole session at
    // the focus-aware cadence (finding 5).
    m_pollTimer.start(pollIntervalMs());
    emit textsChanged();
}

void JitsiCallWindowController::apiLoadFailed()
{
    if (m_embedState != Loading)
        return;
    m_embedState = LoadFailed;
    m_pollTimer.stop();
    emit embedStateChanged();
    emit textsChanged();
}

void JitsiCallWindowController::retry()
{
    if (m_embedState != LoadFailed)
        return;
    m_embedState = Loading;
    emit embedStateChanged();
    emit textsChanged();
}

void JitsiCallWindowController::ensureEmbed()
{
    // The native counterpart of the reference React.memo mount-count contract:
    // the embed surface is created exactly once per open call; the permission
    // policy is granted in the same breath and survives every shell toggle.
    ++m_embedCreations;
    m_permissionsGranted = true;
    if (m_embedFactory)
        m_embedFactory();
    emit embedCreated();
}

void JitsiCallWindowController::setParticipantCountProvider(std::function<int()> provider)
{
    m_countProvider = std::move(provider);
}

void JitsiCallWindowController::setEmbedFactory(std::function<void()> factory)
{
    m_embedFactory = std::move(factory);
}

void JitsiCallWindowController::syncParticipantCount()
{
    // Reference syncCallState: every join/leave event and every reconcile
    // poll re-reads the count from the live embed.
    if (!m_countProvider)
        return;
    const auto count = m_countProvider();
    if (count == m_participantCount)
        return;
    m_participantCount = count;
    emit participantCountChanged();
    emit textsChanged();
}

void JitsiCallWindowController::participantJoined()
{
    syncParticipantCount();
}

void JitsiCallWindowController::participantLeft()
{
    participantJoined();
}

void JitsiCallWindowController::pollNow()
{
    participantJoined();
}

bool JitsiCallWindowController::isMeetingOrigin(const QString &origin) const
{
    // Deny-by-default allowlist (finding 3): a capture/fullscreen grant is
    // only ever issued to the exact origin of the live meeting URL — scheme,
    // host and effective port must all match; everything else (iframes of
    // other sites, http downgrades, host-suffix tricks, same-host frames on a
    // different port) is rejected.
    if (m_mode == Closed || m_roomName.isEmpty() || origin.isEmpty())
        return false;
    const QUrl meeting = QUrl(meetingUrl());
    const QUrl request = QUrl(origin);
    if (meeting.host().isEmpty() || request.host().isEmpty())
        return false;
    return request.scheme().compare(meeting.scheme(), Qt::CaseInsensitive) == 0
        && request.host().compare(meeting.host(), Qt::CaseInsensitive) == 0
        && effectivePort(request) == effectivePort(meeting);
}

void JitsiCallWindowController::syncFullscreen(bool fullscreen)
{
    // The fullscreen control only exists while expanded; the flag follows the
    // real window in both directions (finding 4).
    if (m_mode != Expanded || m_fullscreen == fullscreen)
        return;
    m_fullscreen = fullscreen;
    emit fullscreenChanged();
    emit textsChanged();
}

void JitsiCallWindowController::setPollIntervalMs(int ms)
{
    m_pollIntervalOverrideMs = qMax(0, ms);
    if (m_pollTimer.isActive())
        m_pollTimer.setInterval(pollIntervalMs());
}

QVariantMap JitsiCallWindowController::sessionConfig() const
{
    // jitsi-call.ui.tsx:15-21 — one-time config overwrite, stable across the
    // whole session.
    QVariantMap prejoin;
    prejoin.insert(QStringLiteral("enabled"), false);
    QVariantMap config;
    config.insert(QStringLiteral("startWithAudioMuted"), true);
    config.insert(QStringLiteral("prejoinConfig"), prejoin);
    config.insert(QStringLiteral("startWithVideoMuted"),
        m_call.value(QStringLiteral("startWithVideoMuted"), true).toBool());
    return config;
}

QString JitsiCallWindowController::displayName() const
{
    const auto fromCall = trimmed(m_call, QStringLiteral("displayName"));
    if (!fromCall.isEmpty())
        return fromCall;
    const auto fromLocation = trimmed(m_call, QStringLiteral("locationName"));
    if (!fromLocation.isEmpty())
        return fromLocation;
    return tr("Participant");
}

QString JitsiCallWindowController::callName() const
{
    const auto location = trimmed(m_call, QStringLiteral("locationName"));
    if (!location.isEmpty())
        return location;
    return m_roomName;
}

QString JitsiCallWindowController::meetingUrl() const
{
    return trimmed(m_call, QStringLiteral("meetingUrl"));
}

QString JitsiCallWindowController::parsedDomain() const { return m_domain; }
QString JitsiCallWindowController::parsedRoomName() const { return m_roomName; }

QString JitsiCallWindowController::titleText() const
{
    // resolveJitsiCallHeaderTitle: name wins, then the counted form, then the
    // bare label. Count-form selection at the call site follows the house
    // numerus convention (native-notifications.cpp:597).
    const auto name = callName();
    if (!name.isEmpty())
        return tr("Call") + QStringLiteral(" - ") + name;
    if (m_participantCount >= 0) {
        return m_participantCount == 1
            ? tr("Call (%n participant)", "", m_participantCount)
            : tr("Call (%n participants)", "", m_participantCount);
    }
    return tr("Call");
}

QString JitsiCallWindowController::subtitleText() const
{
    if (m_participantCount < 0)
        return {};
    return m_participantCount == 1
        ? tr("%n participant", "", m_participantCount)
        : tr("%n participants", "", m_participantCount);
}

QString JitsiCallWindowController::stateText() const
{
    switch (m_embedState) {
    case Loading:
        return tr("Loading call client\u2026");
    case LoadFailed:
        return tr("Could not load call client");
    case InvalidUrl:
        return tr("Invalid call link");
    default:
        return {};
    }
}

QString JitsiCallWindowController::retryText() const { return tr("Try again"); }

QString JitsiCallWindowController::fullscreenLabel() const
{
    return m_fullscreen ? tr("Exit fullscreen") : tr("Browser fullscreen mode");
}

QString JitsiCallWindowController::minimizeLabel() const
{
    return m_mode == Minimized ? tr("Expand") : tr("Minimize");
}

QString JitsiCallWindowController::minimizeTooltip() const
{
    return m_mode == Minimized ? tr("Expand") : tr("Minimize to window");
}

QString JitsiCallWindowController::closeLabel() const { return tr("Close call"); }

QString JitsiCallWindowController::embedUrl() const
{
    const auto config = sessionConfig();
    QString hash = QStringLiteral("#config.startWithAudioMuted=true"
                                   "&config.prejoinConfig.enabled=false");
    hash += QStringLiteral("&config.startWithVideoMuted=");
    hash += config.value(QStringLiteral("startWithVideoMuted")).toBool()
        ? QStringLiteral("true") : QStringLiteral("false");
    return meetingUrl() + hash;
}

} // namespace workspace
