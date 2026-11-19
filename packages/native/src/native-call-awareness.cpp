#include "native-call-awareness.h"

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>

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
// with a bounded timeout (the PowerWatcher precedent, issue #78).
const int InhibitCallTimeoutMs = 2000;
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
    // or an in-flight request is never duplicated.
    if (m_cookie || m_inhibitPending || !m_bus.isConnected())
        return;
    m_inhibitPending = true;
    requestInhibit(m_inhibitGeneration, ServiceKind::ScreenSaver);
}

void CallAwareness::requestInhibit(int generation, ServiceKind kind)
{
    const auto target = inhibitTarget(kind);
    auto call = QDBusMessage::createMethodCall(target.service, target.path, target.interface,
        QStringLiteral("Inhibit"));
    call << QCoreApplication::applicationName() << InhibitReason;
    // Async on purpose (PowerWatcher precedent): a blocking call would stall
    // the UI thread behind an unresponsive bus daemon.
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, InhibitCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, generation, kind](QDBusPendingCallWatcher *pending) {
        pending->deleteLater();
        const auto cookie = pending->isError() ? 0u : cookieFromReply(pending->reply());
        if (generation != m_inhibitGeneration) {
            // The call ended (or the guard moved) while this reply was in
            // flight; anything we just learned about gets released at once.
            if (cookie)
                sendUninhibit(kind, cookie);
            return;
        }
        if (!cookie) {
            // The freedesktop chain: modern ScreenSaver first, legacy
            // PowerManagement.Inhibit as the fallback.
            if (kind == ServiceKind::ScreenSaver) {
                requestInhibit(generation, ServiceKind::PowerManagement);
                return;
            }
            m_inhibitPending = false; // neither service answered: sleep stays allowed
            return;
        }
        if (!callActive()) {
            sendUninhibit(kind, cookie); // the call ended after the generation check
            m_inhibitPending = false;
            return;
        }
        m_cookie = cookie;
        m_inhibitService = kind;
        m_inhibitPending = false;
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
