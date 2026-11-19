#include "native-call-awareness.h"

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QTimer>

namespace workspace {
namespace {

const QString ScreenSaverService = QStringLiteral("org.freedesktop.ScreenSaver");
const QString ScreenSaverPath = QStringLiteral("/ScreenSaver");
const QString ScreenSaverInterface = QStringLiteral("org.freedesktop.ScreenSaver");
const QString PmInhibitService = QStringLiteral("org.freedesktop.PowerManagement.Inhibit");
const QString PmInhibitPath = QStringLiteral("/org/freedesktop/PowerManagement/Inhibit");
const QString PmInhibitInterface = QStringLiteral("org.freedesktop.PowerManagement.Inhibit");
const QString InhibitReason = QStringLiteral("Workspace call in progress");
// Keep the UI thread off the bus entirely: every inhibit round-trip is async
// with a bounded wait (the PowerWatcher precedent, issue #78).
const int InhibitCallTimeoutMs = 2000;
// No D-Bus-level timeout may drop the reply before we can see the cookie it
// grants (issue #86 review): a granted inhibitor without a known cookie can
// never be released, so the call itself only waits InhibitCallTimeoutMs and
// the watcher stays armed far longer to catch and release a late grant.
const int InhibitReplyGraceMs = 24 * 60 * 60 * 1000; // one day, in ms
constexpr int MaxRoomLength = 128;

struct InhibitTarget
{
    QString service;
    QString path;
    QString interface;
    QString uninhibitMethod;
};

InhibitTarget inhibitTarget(CallAwareness::ServiceKind kind)
{
    switch (kind) {
    case CallAwareness::ServiceKind::ScreenSaver:
        return {ScreenSaverService, ScreenSaverPath, ScreenSaverInterface,
            QStringLiteral("UnInhibit")};
    case CallAwareness::ServiceKind::PowerManagement:
        return {PmInhibitService, PmInhibitPath, PmInhibitInterface, QStringLiteral("Uninhibit")};
    }
    return {};
}

// Inhibit answers uint32 ('u'); a 0 cookie always means "service missing,
// errored or non-compliant", which is also how the callers treat it.
quint32 cookieFromReply(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return 0;
    const auto cookie = reply.arguments().first();
    if (cookie.userType() != QMetaType::UInt)
        return 0;
    return cookie.toUInt();
}

} // namespace

CallAwareness::CallAwareness(QObject *parent) : QObject(parent) {}

CallAwareness::~CallAwareness()
{
    releaseInhibit();
}

void CallAwareness::start(const QDBusConnection &bus)
{
    if (m_started)
        return;
    m_started = true;
    // An unconnected bus is not an error: inhibit becomes a no-op and the
    // tooltip state machine keeps working, the same degrade posture the
    // reference power paths use (PowerWatcher, issue #78).
    m_bus = bus;
}

QString CallAwareness::sanitizeRoomName(const QString &raw)
{
    // Port of main.ts:1270-1273: strip [\x00-\x1f\x7f] first, then cap at
    // 128 chars; the empty result falls back to "Call". Whitespace is kept -
    // the reference does not trim.
    QString clean;
    clean.reserve(raw.size());
    for (const auto character : raw) {
        const auto code = character.unicode();
        if (code <= 0x1f || code == 0x7f)
            continue;
        clean.append(character);
    }
    const auto capped = clean.left(MaxRoomLength);
    return capped.isEmpty() ? QStringLiteral("Call") : capped;
}

void CallAwareness::callStarted(const QString &room, int participants)
{
    // Reference call:start: sanitize the room, acquire the display-sleep
    // inhibitor exactly once, repaint the tooltip. The payload participant
    // count is not rendered at start - only callParticipantsUpdated ever
    // publishes one (reference parity, issue acceptance).
    Q_UNUSED(participants);
    m_room = sanitizeRoomName(room);
    m_participants = -1;
    acquireInhibit();
    emit callStateChanged(m_room, m_participants);
}

void CallAwareness::callParticipantsUpdated(int count)
{
    // Reference call:update guards on the active room: while idle it does
    // nothing at all.
    if (!callActive())
        return;
    m_participants = count;
    emit callStateChanged(m_room, m_participants);
}

void CallAwareness::callEnded()
{
    m_room.clear();
    m_participants = -1;
    // Invalidate any in-flight Inhibit reply before releasing, so a late
    // cookie can never outlive the call.
    ++m_inhibitGeneration;
    releaseInhibit();
    emit callStateChanged(QString(), -1);
}

void CallAwareness::acquireInhibit()
{
    // Exactly once per call (the reference blocker id guard): a held cookie
    // or an in-flight request of this generation is never duplicated. A
    // request still in flight from an earlier, already ended call does not
    // block this one - its reply counts as stale and releases itself (issue
    // #86 review, finding 1).
    if (m_cookie || m_inhibitPendingGeneration == m_inhibitGeneration || !m_bus.isConnected())
        return;
    requestInhibit(m_inhibitGeneration, ServiceKind::ScreenSaver);
}

void CallAwareness::requestInhibit(int generation, ServiceKind kind)
{
    m_inhibitPendingGeneration = generation;
    m_inhibitPendingKind = kind;
    const auto target = inhibitTarget(kind);
    auto call = QDBusMessage::createMethodCall(target.service, target.path, target.interface,
        QStringLiteral("Inhibit"));
    call << QCoreApplication::applicationName() << InhibitReason;
    // Async on purpose (PowerWatcher precedent): a blocking call would stall
    // the UI thread behind an unresponsive bus daemon. The round-trip is not
    // cut short by a D-Bus timeout: a grant whose reply was discarded could
    // never be released without its cookie (issue #86 review, findings 2/3),
    // so the watcher waits out the whole grace period and the bounded call
    // deadline is driven by the timer below instead.
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, InhibitReplyGraceMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, generation, kind](QDBusPendingCallWatcher *pending) {
        pending->deleteLater();
        const auto cookie = pending->isError() ? 0u : cookieFromReply(pending->reply());
        const bool awaited = m_inhibitPendingGeneration == generation
            && m_inhibitPendingKind == kind;
        if (!awaited) {
            // The call ended, or this reply landed after the deadline dropped
            // the request as late. A grant nobody is waiting for is released
            // at once, so an inhibitor can never outlive the request that
            // asked for it - and the pending marker of a newer request stays
            // put. One exception (issue #86 review, finding 4627245017): a
            // late grant whose call is still active is that call's only
            // inhibitor when the fallback chain granted nothing - throwing
            // it back would leave the active call uninhibited. Retain it and
            // let callEnded release it normally.
            if (cookie) {
                const bool lateForLiveCall = generation == m_inhibitGeneration
                    && callActive();
                if (lateForLiveCall && !m_cookie) {
                    m_cookie = cookie;
                    m_inhibitService = kind;
                    return;
                }
                sendUninhibit(kind, cookie);
            }
            return;
        }
        m_inhibitPendingGeneration = -1;
        m_inhibitPendingKind = ServiceKind::None;
        if (!cookie) {
            // The freedesktop chain: modern ScreenSaver first, legacy
            // PowerManagement.Inhibit as the fallback. A transport error (the
            // usual cause here) is answered at once, so no deadline is spent.
            if (kind == ServiceKind::ScreenSaver) {
                requestInhibit(generation, ServiceKind::PowerManagement);
                return;
            }
            return; // neither service answered: sleep stays allowed
        }
        if (!callActive()) {
            sendUninhibit(kind, cookie); // the call ended after the guard check
            return;
        }
        if (m_cookie) {
            // Defensive (real buses may answer out of order): a late reply
            // from the other service was already adopted as this call's
            // inhibitor, so this awaited grant is the duplicate and goes
            // straight back. The mock fixture dispatches both services on
            // the test thread, where this ordering cannot reproduce.
            sendUninhibit(kind, cookie);
            return;
        }
        m_cookie = cookie;
        m_inhibitService = kind;
    });
    // The bounded wait the UI never pays for: at the deadline the request
    // stops being awaited - ScreenSaver falls through to the legacy service,
    // PowerManagement gives up - while the watcher above stays armed to catch
    // and release a grant that still arrives late.
    QTimer::singleShot(InhibitCallTimeoutMs, this, [this, generation, kind] {
        if (m_inhibitPendingGeneration != generation || m_inhibitPendingKind != kind)
            return; // the reply already landed, or the guard moved on
        m_inhibitPendingGeneration = -1;
        m_inhibitPendingKind = ServiceKind::None;
        if (generation != m_inhibitGeneration)
            return; // the call ended meanwhile: nothing left to fall back for
        if (kind == ServiceKind::ScreenSaver)
            requestInhibit(generation, ServiceKind::PowerManagement);
    });
}

void CallAwareness::releaseInhibit()
{
    if (!m_cookie)
        return;
    const auto cookie = *m_cookie;
    const auto service = m_inhibitService;
    m_cookie.reset();
    m_inhibitService = ServiceKind::None;
    sendUninhibit(service, cookie);
}

void CallAwareness::sendUninhibit(ServiceKind kind, quint32 cookie)
{
    if (kind == ServiceKind::None || !m_bus.isConnected())
        return;
    const auto target = inhibitTarget(kind);
    auto call = QDBusMessage::createMethodCall(target.service, target.path, target.interface,
        target.uninhibitMethod);
    call << cookie;
    // Fire and forget: UnInhibit has no payload worth waiting for, and the
    // session bus drops the cookie anyway if the client disappears first.
    m_bus.asyncCall(call, InhibitCallTimeoutMs);
}

} // namespace workspace
