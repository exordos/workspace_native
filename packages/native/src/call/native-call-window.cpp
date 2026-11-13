#include "call/native-call-window.h"

#include "call/jitsi-call.h"

namespace workspace {

namespace {
QString trimmed(const QVariantMap &call, const QString &key)
{
    return call.value(key).toString().trimmed();
}
} // namespace

QString JitsiCallWindowController::allowPolicy()
{
    // jitsi-call-permissions.lib.ts:4 — applied once when the embed surface
    // is created; shell toggles must not look like a new session.
    return QStringLiteral("camera; microphone; fullscreen; display-capture");
}

JitsiCallWindowController::JitsiCallWindowController(QObject *parent) : QObject(parent) {}

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

QRect JitsiCallWindowController::bounds() const
{
    return m_mode == Minimized ? m_pipBounds : expandedBounds(m_viewport);
}

int JitsiCallWindowController::pollIntervalMs() const
{
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
}

void JitsiCallWindowController::setPipBounds(const QRect &bounds)
{
    // The PiP surface stays resizable down to the shared 280x180 minimum.
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
    m_fullscreen = false;
    m_permissionsGranted = false;
    m_participantCount = -1;
    m_embedState = m_roomName.isEmpty() ? InvalidUrl : Loading;
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
    emit textsChanged();
}

void JitsiCallWindowController::apiLoadFailed()
{
    if (m_embedState != Loading)
        return;
    m_embedState = LoadFailed;
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
