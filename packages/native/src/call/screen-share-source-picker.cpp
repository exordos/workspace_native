#include "call/screen-share-source-picker.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace workspace {

namespace {

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr auto kRequestInterface = "org.freedesktop.portal.Request";
constexpr auto kSessionInterface = "org.freedesktop.portal.Session";

QString handleToken()
{
    return QStringLiteral("workspace_") + QUuid::createUuid().toString(QUuid::Id128);
}

QString portalObjectPath(const QString &kind, const QString &token)
{
    QString sender = QDBusConnection::sessionBus().baseService();
    sender.remove(0, 1);
    sender.replace('.', '_');
    return QStringLiteral("/org/freedesktop/portal/desktop/%1/%2/%3").arg(kind, sender, token);
}

void closePortalObject(const QString &path, const char *interface)
{
    if (path.isEmpty())
        return;
    QDBusConnection::sessionBus().send(QDBusMessage::createMethodCall(
        QLatin1String(kPortalService), path, QLatin1String(interface), QStringLiteral("Close")));
}

} // namespace

ScreenShareSourcePicker::ScreenShareSourcePicker(QObject *parent)
    : QObject(parent)
{
}

ScreenShareSourcePicker::~ScreenShareSourcePicker()
{
    finishCurrentExchange();
    releaseSource();
}

void ScreenShareSourcePicker::releaseSource()
{
    closePortalObject(m_selectedSession, kSessionInterface);
    m_selectedSession.clear();
    if (m_portalExchangeActive)
        failCurrentExchange(tr("The screen share source selection was cancelled."));
}

void ScreenShareSourcePicker::setPortalProbe(PortalProbe probe)
{
    m_portalProbe = std::move(probe);
}

void ScreenShareSourcePicker::setPortalRequest(PortalRequest request)
{
    m_portalRequest = std::move(request);
}

void ScreenShareSourcePicker::setPortalTimeoutMs(int ms)
{
    if (ms > 0)
        m_portalTimeoutMs = ms;
}

bool ScreenShareSourcePicker::portalAvailable() const
{
    if (m_portalProbe)
        return m_portalProbe();
    return defaultPortalProbe();
}

bool ScreenShareSourcePicker::defaultPortalProbe() const
{
    const auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    return bus.interface()->isServiceRegistered(QLatin1String(kPortalService));
}

void ScreenShareSourcePicker::requestSource()
{
    if (m_portalExchangeActive)
        return; // one portal request at a time
    if (!portalAvailable()) {
        // Deterministic fallback: Qt's own screen model, first screen.
        const auto sources = screenModelSources();
        if (sources.isEmpty()) {
            emit sourceFailed(tr("No display is available to share."));
            return;
        }
        emit sourceSelected(toMap(sources.first()));
        return;
    }
    if (m_portalRequest) {
        Source source;
        QString error;
        if (m_portalRequest(source, error))
            emit sourceSelected(toMap(source));
        else
            emit sourceFailed(error.isEmpty()
                ? tr("The system screen picker could not be opened.")
                : error);
        return;
    }
    startDefaultPortalExchange();
}

void ScreenShareSourcePicker::startDefaultPortalExchange()
{
    m_portalExchangeActive = true;
    const quint64 serial = ++m_exchangeSerial;
    m_portalStep = Step::CreateSession;
    const auto sessionToken = handleToken();
    m_sessionPath = portalObjectPath(QStringLiteral("session"), sessionToken);
    // One timer per exchange (finding 3): it is owned by this serial and is
    // stopped and destroyed when the exchange completes, so it can never fire
    // against a later exchange the way the previous shared timer did. The
    // timeout bounds one whole exchange: every portal round trip plus the
    // user's pick inside the portal dialog.
    armPortalTimeout(serial);
    callPortalStep(serial, "CreateSession", {QVariantMap{
        {QStringLiteral("session_handle_token"), sessionToken}}});
}

void ScreenShareSourcePicker::callPortalStep(
    quint64 serial, const char *method, const QVariantList &args)
{
    const QString token = handleToken();
    const QString expectedPath = portalObjectPath(QStringLiteral("request"), token);
    if (!subscribePortalRequest(expectedPath)) {
        failCurrentExchange(tr("The system screen picker could not be opened."));
        return;
    }
    // Subscribe before sending: the portal can emit Response immediately.
    auto arguments = args;
    auto options = arguments.last().toMap();
    options.insert(QStringLiteral("handle_token"), token);
    arguments.last() = options;
    auto call = QDBusMessage::createMethodCall(QLatin1String(kPortalService),
        QLatin1String(kPortalPath), QLatin1String(kScreenCastInterface), QLatin1String(method));
    call.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, m_portalTimeoutMs), QCoreApplication::instance());
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
        [picker = QPointer(this), serial, expectedPath](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        const auto path = reply->reply().arguments().value(0).value<QDBusObjectPath>().path();
        if (!picker || !picker->isCurrentExchange(serial) || picker->m_requestPath != expectedPath) {
            // Release may run before the portal discloses an alternate Request.
            if (path != expectedPath)
                closePortalObject(path, kRequestInterface);
            return;
        }
        if (reply->isError() || path.isEmpty()) {
            picker->failCurrentExchange(tr("The system screen picker could not be opened."));
            return;
        }
        picker->m_requestPath = path;
        picker->m_waitingForRequestPath = false;
        const auto early = std::exchange(picker->m_earlyResponses, {});
        const auto response = early.constFind(path);
        if (response != early.cend())
            picker->handlePortalResult(response->first, response->second);
    });
}

bool ScreenShareSourcePicker::subscribePortalRequest(const QString &path)
{
    if (!QDBusConnection::sessionBus().connect(QLatin1String(kPortalService), {},
            QLatin1String(kRequestInterface), QStringLiteral("Response"), this,
            SLOT(onPortalResponse(uint,QVariantMap,QDBusMessage))))
        return false;
    m_requestPath = path;
    m_waitingForRequestPath = true;
    return true;
}

void ScreenShareSourcePicker::unsubscribePortalRequest()
{
    if (m_requestPath.isEmpty())
        return;
    QDBusConnection::sessionBus().disconnect(QLatin1String(kPortalService), {},
        QLatin1String(kRequestInterface), QStringLiteral("Response"), this,
        SLOT(onPortalResponse(uint,QVariantMap,QDBusMessage)));
    m_requestPath.clear();
    m_waitingForRequestPath = false;
    m_earlyResponses.clear();
}

void ScreenShareSourcePicker::armPortalTimeout(quint64 serial)
{
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, serial] {
        if (!isCurrentExchange(serial))
            return;
        failCurrentExchange(tr("The screen share source selection timed out."));
    });
    m_portalTimer = timer;
    timer->start(m_portalTimeoutMs);
}

void ScreenShareSourcePicker::onPortalResponse(uint code, const QVariantMap &results,
    const QDBusMessage &signal)
{
    if (!m_portalExchangeActive)
        return;
    if (signal.path() != m_requestPath) {
        // Older portals can choose a different Request path. A Response may
        // precede the method reply; only the path confirmed by that reply wins.
        if (m_waitingForRequestPath && m_earlyResponses.size() < 32
            && signal.path().startsWith(QStringLiteral("/org/freedesktop/portal/desktop/request/")))
            m_earlyResponses.insert(signal.path(), {code, results});
        return;
    }
    handlePortalResult(code, results);
}

void ScreenShareSourcePicker::handlePortalResult(uint code, const QVariantMap &results)
{
    unsubscribePortalRequest();
    if (code != 0) {
        failCurrentExchange(code == 1 ? tr("The screen share source selection was cancelled.")
                                      : tr("The system screen picker could not be opened."));
        return;
    }
    switch (m_portalStep) {
    case Step::CreateSession: {
        const auto session = results.value(QStringLiteral("session_handle")).toString();
        if (session.isEmpty()) {
            failCurrentExchange(tr("The system screen picker could not be opened."));
            return;
        }
        m_sessionPath = session;
        m_portalStep = Step::SelectSources;
        callPortalStep(m_exchangeSerial, "SelectSources", {QDBusObjectPath(m_sessionPath),
            QVariantMap{{QStringLiteral("types"), 3u}}});
        return;
    }
    case Step::SelectSources:
        m_portalStep = Step::Start;
        callPortalStep(m_exchangeSerial, "Start", {QDBusObjectPath(m_sessionPath), QString(), QVariantMap{}});
        return;
    case Step::Start: {
        auto stream = streamFromResult(results);
        if (stream.isEmpty()) {
            failCurrentExchange(tr("The screen share result did not include a selectable stream."));
            return;
        }
        closePortalObject(m_selectedSession, kSessionInterface);
        m_selectedSession = std::exchange(m_sessionPath, {});
        Source source;
        source.id = QStringLiteral("portal-stream");
        source.name = tr("Screen capture");
        source.uri = QStringLiteral("pipewire:%1").arg(stream.value(QStringLiteral("nodeId")).toUInt());
        source.fromPortal = true;
        source.live = true;
        stream.insert(toMap(source));
        stream.insert(QStringLiteral("sessionHandle"), m_selectedSession);
        finishCurrentExchange();
        emit sourceSelected(stream);
        return;
    }
    }
}

bool ScreenShareSourcePicker::isCurrentExchange(quint64 serial) const
{
    return m_portalExchangeActive && serial == m_exchangeSerial;
}

void ScreenShareSourcePicker::failCurrentExchange(const QString &reason)
{
    finishCurrentExchange();
    emit sourceFailed(reason);
}

void ScreenShareSourcePicker::finishCurrentExchange()
{
    // Completion first disarms the exchange: the timer is stopped and
    // destroyed and the Request subscription is dropped here, so neither can
    // ever fire against the next exchange (finding 3).
    m_portalExchangeActive = false;
    if (m_portalTimer) {
        m_portalTimer->stop();
        m_portalTimer->deleteLater();
        m_portalTimer = nullptr;
    }
    closePortalObject(m_requestPath, kRequestInterface);
    unsubscribePortalRequest();
    closePortalObject(m_sessionPath, kSessionInterface);
    m_sessionPath.clear();
    m_portalStep = Step::CreateSession;
}

QList<ScreenShareSourcePicker::Source> ScreenShareSourcePicker::screenModelSources()
{
    QList<Source> sources;
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        Source source;
        source.id = QStringLiteral("screen-%1").arg(i);
        source.name = tr("Screen %1").arg(i + 1);
        const QRect geometry = screens.at(i)->geometry();
        source.uri = QStringLiteral("screen:%1?geometry=%2,%3,%4,%5")
            .arg(i)
            .arg(geometry.x()).arg(geometry.y())
            .arg(geometry.width()).arg(geometry.height());
        source.fromPortal = false;
        source.live = true; // a monitor reference, streamable by definition
        sources.append(source);
    }
    return sources;
}

QVariantMap ScreenShareSourcePicker::streamFromResult(const QVariantMap &results)
{
    using Streams = QList<std::pair<quint32, QVariantMap>>;
    QVariant raw = results.value(QStringLiteral("streams"));
    if (raw.metaType() == QMetaType::fromType<QDBusVariant>())
        raw = raw.value<QDBusVariant>().variant();
    if (raw.metaType() == QMetaType::fromType<QDBusArgument>()
        && raw.value<QDBusArgument>().currentSignature() != QStringLiteral("a(ua{sv})"))
        return {};
    const auto streams = qdbus_cast<Streams>(raw);
    if (streams.isEmpty())
        return {};
    return {{QStringLiteral("nodeId"), streams.first().first},
        {QStringLiteral("properties"), streams.first().second}};
}

QVariantMap ScreenShareSourcePicker::toMap(const Source &source)
{
    return QVariantMap{
        {QStringLiteral("id"), source.id},
        {QStringLiteral("name"), source.name},
        {QStringLiteral("uri"), source.uri},
        {QStringLiteral("fromPortal"), source.fromPortal},
        {QStringLiteral("live"), source.live},
    };
}

} // namespace workspace
