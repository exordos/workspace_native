#include "call/native-call-window.h"

#include "call/display-capture-policy.h"
#include "call/jitsi-call.h"

#include <QCoreApplication>
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

QString JitsiCallWindowController::standaloneBackgroundColor()
{
    // workspace_ui e0194ffb jitsi-call-window.ts:244.
    return QStringLiteral("#1B1B1D");
}

QString JitsiCallWindowController::standaloneBackground() const
{
    return standaloneBackgroundColor();
}

QString JitsiCallWindowController::standaloneTitle() const
{
    // options.appName in the reference BrowserWindow construction; the same
    // application-name fallback the notifications layer uses.
    const auto name = QCoreApplication::applicationName();
    return name.isEmpty() ? QStringLiteral("Exordos Workspace") : name;
}

JitsiCallWindowController::JitsiCallWindowController(QObject *parent) : QObject(parent)
{
    // Focus-aware reconcile poll (jitsi-participant-count.hook.ts: start on
    // apiReady, restart on focus change, clear on close): the tick re-reads
    // the injected count provider exactly like a manual pollNow.
    m_pollTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &JitsiCallWindowController::pollNow);
    // Issue #728: a call that opens but never reports an api load outcome
    // (e.g. the embed component failed to instantiate at all) must leave
    // Loading on a finite deadline so the shell can surface its retry state.
    m_embedDeadlineTimer.setSingleShot(true);
    connect(&m_embedDeadlineTimer, &QTimer::timeout,
            this, &JitsiCallWindowController::embedDeadlineElapsed);
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
    const bool wasVisible = windowVisible();
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
    // #846: a fresh session starts hidden (the standalone window is shown
    // only once the embed finished loading).
    m_windowShown = false;
    m_embedState = m_roomName.isEmpty() ? InvalidUrl : Loading;
    m_pollTimer.stop();
    if (m_embedState == Loading)
        m_embedDeadlineTimer.start(m_embedDeadlineMs);
    else
        m_embedDeadlineTimer.stop();
    m_mode = Expanded;
    emit modeChanged();
    emit boundsChanged();
    emit embedStateChanged();
    emit fullscreenChanged();
    emit participantCountChanged();
    emit textsChanged();
    emit callIdChanged();
    if (windowVisible() != wasVisible)
        emit windowVisibilityChanged();
}

QString JitsiCallWindowController::callId() const
{
    // The store mints a uuid callId per active call (reference
    // jitsi-call.model.ts:80); a direct open() payload without one falls back
    // to the call key and then the meeting URL so the identity is stable and
    // never empty while a call is open.
    const auto explicitId = m_call.value(QStringLiteral("callId")).toString();
    if (!explicitId.isEmpty())
        return explicitId;
    const auto callKey = m_call.value(QStringLiteral("callKey")).toString();
    if (!callKey.isEmpty())
        return callKey;
    return m_call.value(QStringLiteral("meetingUrl")).toString();
}

QVariantMap JitsiCallWindowController::sanitizeShellPayload(const QVariantMap &payload,
    QString *rejectReason)
{
    const auto reject = [rejectReason](const QString &reason) {
        if (rejectReason)
            *rejectReason = reason;
        return QVariantMap();
    };
    const static QString badCallId = QStringLiteral(
        "callId must be a non-empty string of at most 128 characters");
    // callId (jitsi-call-window.ts:46-48): explicit id first, then the
    // derived identity (same chain as callId()).
    QString callId = payload.value(QStringLiteral("callId")).toString();
    if (callId.isEmpty())
        callId = payload.value(QStringLiteral("callKey")).toString();
    if (callId.isEmpty())
        callId = payload.value(QStringLiteral("meetingUrl")).toString();
    if (callId.isEmpty() || callId.size() > kMaxCallIdLength)
        return reject(badCallId);
    const auto meetingUrl = payload.value(QStringLiteral("meetingUrl")).toString();
    // readPayload treats an absent/blank meetingUrl as fatal; the native
    // shell instead owns it in the InvalidUrl state (invalid-link notice),
    // matching the shipped #263/#728 contract, so only the shape bounds of a
    // PRESENT url are enforced here.
    if (!meetingUrl.isEmpty()) {
        if (meetingUrl.size() > kMaxMeetingUrlLength)
            return reject(QStringLiteral("meetingUrl exceeds 8192 characters"));
        const QUrl url(meetingUrl, QUrl::StrictMode);
        auto path = url.path(QUrl::FullyEncoded);
        path.remove(QLatin1Char('/'));
        if ((url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))
            || !url.userName().isEmpty() || !url.password().isEmpty()
            || path.isEmpty()) {
            return reject(QStringLiteral(
                "meetingUrl must be an http(s) URL without userinfo and with a non-empty path"));
        }
    }
    QVariantMap sanitized = payload;
    sanitized.insert(QStringLiteral("callId"), callId);
    // locationName/displayName truncated to 256 (:81-84).
    const auto locationName = payload.value(QStringLiteral("locationName")).toString();
    sanitized.insert(QStringLiteral("locationName"), locationName.left(kMaxTextLength));
    const auto displayName = payload.value(QStringLiteral("displayName")).toString();
    sanitized.insert(QStringLiteral("displayName"), displayName.left(kMaxTextLength));
    // Invalid theme is DROPPED, never a rejection (:77-80).
    if (sanitized.contains(QStringLiteral("theme"))) {
        const auto theme = sanitized.value(QStringLiteral("theme")).toMap();
        const auto mode = theme.value(QStringLiteral("mode")).toString();
        const auto paletteId = theme.value(QStringLiteral("paletteId")).toString();
        if ((mode != QLatin1String("light") && mode != QLatin1String("dark"))
            || paletteId.isEmpty() || paletteId.size() > kMaxPaletteIdLength) {
            sanitized.remove(QStringLiteral("theme"));
        }
    }
    return sanitized;
}

bool JitsiCallWindowController::requestStandaloneOpen(const QVariantMap &activeCall)
{
    QString reason;
    const QVariantMap sanitized = sanitizeShellPayload(activeCall, &reason);
    if (sanitized.isEmpty()) {
        emit openRejected(reason);
        return false;
    }
    if (m_mode != Closed) {
        // Single-window rules (jitsi-call-window.ts:228-231): re-opening the
        // same call focuses the existing window; a different callId is
        // refused while a window is open and the live window is untouched.
        if (sanitized.value(QStringLiteral("callId")).toString() == callId()) {
            emit focusRequested();
            return true;
        }
        emit openRejected(QStringLiteral("A different Jitsi call window is already open"));
        return false;
    }
    open(sanitized);
    return true;
}

void JitsiCallWindowController::close()
{
    if (m_mode == Closed)
        return;
    const bool wasVisible = windowVisible();
    // #846: the ':closed' announcement to the main window happens only if the
    // call window had actually loaded (reference loaded guard, the main-side
    // call state / tray tooltip / sleep inhibit reset rides on it).
    const bool wasLoaded = m_windowShown;
    const QString closingCallId = callId();
    m_mode = Closed;
    m_embedState = Idle;
    m_permissionsGranted = false;
    m_participantCount = -1;
    m_fullscreen = false;
    m_windowShown = false;
    m_pollTimer.stop();
    m_embedDeadlineTimer.stop();
    emit modeChanged();
    emit boundsChanged();
    emit embedStateChanged();
    emit permissionsChanged();
    emit fullscreenChanged();
    emit participantCountChanged();
    emit textsChanged();
    emit closeRequested();
    if (windowVisible() != wasVisible)
        emit windowVisibilityChanged();
    if (wasLoaded)
        emit standaloneClosed(closingCallId);
}

void JitsiCallWindowController::toggleMinimize()
{
    if (m_mode == Closed)
        return;
    const bool wasVisible = windowVisible();
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
    // #846: the standalone surface only exists while expanded (minimize hands
    // presentation to the PiP tool window), so the visibility gate flips.
    if (windowVisible() != wasVisible)
        emit windowVisibilityChanged();
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
    m_embedDeadlineTimer.stop();
    m_embedState = Ready;
    // #846: the successful load is the exact moment the reference shows the
    // standalone window (shown only after the page loaded).
    m_windowShown = true;
    emit embedStateChanged();
    emit windowVisibilityChanged();
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
    m_embedDeadlineTimer.stop();
    m_embedState = LoadFailed;
    m_pollTimer.stop();
    emit embedStateChanged();
    emit textsChanged();
}

// Issue #728: the finite settle for a load that reported nothing at all.
void JitsiCallWindowController::embedDeadlineElapsed()
{
    apiLoadFailed();
}

void JitsiCallWindowController::retry()
{
    if (m_embedState != LoadFailed)
        return;
    m_embedState = Loading;
    m_embedDeadlineTimer.start(m_embedDeadlineMs);
    emit embedStateChanged();
    emit textsChanged();
    // #792: the state flip alone changes nothing observable for the Loader
    // (active through both states, status stuck at Error); ask the shell to
    // genuinely re-evaluate the embed source.
    emit embedRetryRequested();
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

bool JitsiCallWindowController::isDisplayCaptureRequestAllowed(bool isMainFrame,
    const QString &requestingUrl) const
{
    // The frame model is deliberately minimal (see the header note): the
    // embed surface loads only the meeting document, so the subtree is that
    // one frame and only while it is live - an embed that never reached
    // Ready, a failed load, or a closed call leaves no live frame and every
    // request denies. The https rule and the main-frame denial are the pure
    // predicate's, unchanged from the reference.
    QList<DisplayCaptureFrame> frames;
    if (m_mode != Closed && m_embedState == Ready && !m_roomName.isEmpty()) {
        DisplayCaptureFrame meetingFrame;
        meetingFrame.url = QUrl(meetingUrl());
        frames.append(meetingFrame);
    }
    return isAllowedDisplayCapture(isMainFrame, QUrl(requestingUrl), frames);
}

bool JitsiCallWindowController::isDisplayCaptureRequestTrustworthy(
    const QString &requestingUrl) const
{
    // Round 2 (findings 1/2): FAIL CLOSED. Qt 6.10.2's QWebEnginePermission
    // exposes only origin and permissionType (qwebenginepermission.h:30-33,
    // 70-71) - no frame object, no main-frame flag, no frame tree - and this
    // view loads the meeting URL as its own main document, so a request from
    // the meeting main frame carries exactly the meeting origin. An origin
    // match therefore cannot distinguish the allowed child-frame request the
    // reference gate proves (workspace_ui main.ts:996-1020 denies unless the
    // request is verifiably from a live secure child frame) from the denied
    // main-frame request: the only sound policy on this runtime is to deny
    // every request whose frame identity cannot be verified, which is all of
    // them. The first round's wiring that fed the origin into the modeled
    // child-frame check fabricated the identity (always claiming
    // isMainFrame=false) and granted main-frame requests - that is exactly
    // the scenario findings 1/2 condemned, so the modeled gate above stays
    // unit-tested as the policy reference but no production request reaches
    // it until a trustworthy frame identity signal exists.
    Q_UNUSED(requestingUrl)
    return false;
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

JitsiEmbedBridge::JitsiEmbedBridge(QObject *parent) : QObject(parent) {}

void JitsiEmbedBridge::reportParticipantCount(int count)
{
    if (m_liveCount == count)
        return;
    m_liveCount = count;
    emit liveParticipantCountChanged();
}

void JitsiEmbedBridge::reportRoster(const QStringList &displayNames)
{
    if (m_roster == displayNames)
        return;
    m_roster = displayNames;
    emit liveRosterChanged();
}

void JitsiEmbedBridge::handleParticipantsJoined()
{
    // Reference syncCallState: the event only triggers a re-read of the
    // live count; the bridge is where that count lives.
    if (m_controller)
        m_controller->participantJoined();
}

void JitsiEmbedBridge::handleParticipantsLeft()
{
    handleParticipantsJoined();
}

void JitsiEmbedBridge::attachTo(JitsiCallWindowController *controller)
{
    m_controller = controller;
    controller->setParticipantCountProvider([this] { return m_liveCount; });
    // Each live count report flows into the shell the same way a reference
    // participantJoined/participantLeft listener re-syncs it.
    connect(this, &JitsiEmbedBridge::liveParticipantCountChanged,
            controller, &JitsiCallWindowController::participantJoined);
    // Closing the call resets the count and the shared roster (reference
    // jitsi-participant-count.hook.ts:34-41 and shell close effect).
    connect(controller, &JitsiCallWindowController::closeRequested, this, [this] {
        m_liveCount = -1;
        m_roster.clear();
        emit liveParticipantCountChanged();
        emit liveRosterChanged();
    });
}

} // namespace workspace
