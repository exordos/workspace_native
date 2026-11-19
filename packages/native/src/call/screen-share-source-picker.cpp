#include "call/screen-share-source-picker.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

namespace workspace {

namespace {

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr auto kRequestInterface = "org.freedesktop.portal.Request";

// One value out of the portal reply, whatever wrapper the D-Bus demarshal put
// on it: values inside a variant arrive as QDBusVariant and structured
// members arrive as a QDBusArgument stream.
QString extractPortalString(const QVariant &raw)
{
    QVariant value = raw;
    while (true) {
        if (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
            value = value.value<QDBusVariant>().variant();
            continue;
        }
        if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
            QDBusArgument argument = value.value<QDBusArgument>();
            if (argument.currentType() == QDBusArgument::ArrayType) {
                // An ao/as member: the first non-empty element wins, exactly
                // the portal's single-selection usage.
                QString first;
                argument.beginArray();
                while (!argument.atEnd() && first.isEmpty()) {
                    QVariant item;
                    argument >> item;
                    first = extractPortalString(item);
                }
                argument.endArray();
                return first;
            }
            value = argument.asVariant();
            continue;
        }
        if (value.typeId() == QMetaType::QString)
            return value.toString();
        if (value.typeId() == QMetaType::QStringList) {
            const QStringList list = value.toStringList();
            return list.isEmpty() ? QString() : list.first();
        }
        if (value.metaType() == QMetaType::fromType<QDBusObjectPath>())
            return value.value<QDBusObjectPath>().path();
        return {};
    }
}

} // namespace

ScreenShareSourcePicker::ScreenShareSourcePicker(QObject *parent)
    : QObject(parent)
{
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
    m_sessionPath.clear();
    // One timer per exchange (finding 3): it is owned by this serial and is
    // stopped and destroyed when the exchange completes, so it can never fire
    // against a later exchange the way the previous shared timer did. The
    // timeout bounds one whole exchange: every portal round trip plus the
    // user's pick inside the portal dialog.
    armPortalTimeout(serial);
    // The live source-selection contract (finding 4), step 1: a ScreenCast
    // session, whose Request Response answers with "session_target". The
    // parent_window stays empty (no toplevel identity to advertise yet) and
    // handle_token is the opaque per-exchange correlation id.
    callPortalStep(serial, "CreateSession", {QVariantMap{
        {QStringLiteral("handle_token"),
            QStringLiteral("wsnative87-%1").arg(serial)}}});
}

void ScreenShareSourcePicker::callPortalStep(
    quint64 serial, const char *method, const QVariantList &args)
{
    if (!isCurrentExchange(serial))
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(kPortalService),
        QLatin1String(kPortalPath),
        QLatin1String(kScreenCastInterface),
        QLatin1String(method));
    call.setArguments(args);
    // QDBusMessage has no per-message timeout: the deadline rides the
    // connection call itself, bounded by the exchange-wide timer. The
    // exchange stays asynchronous so the calling thread never spins a nested
    // event loop.
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, m_portalTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, serial, watcher](QDBusPendingCallWatcher *self) {
        watcher->deleteLater();
        if (!isCurrentExchange(serial))
            return;
        if (self->isError()) {
            failCurrentExchange(tr("The system screen picker could not be opened."));
            return;
        }
        const QDBusMessage reply = self->reply();
        QString requestPath;
        if (!reply.arguments().isEmpty())
            requestPath = reply.arguments().first().value<QDBusObjectPath>().path();
        if (requestPath.isEmpty()) {
            failCurrentExchange(tr("The system screen picker could not be opened."));
            return;
        }
        subscribePortalRequest(serial, requestPath);
    });
}

void ScreenShareSourcePicker::subscribePortalRequest(
    quint64 serial, const QString &requestPath)
{
    Q_UNUSED(serial)
    // The subscription belongs to this exchange (finding 3): it is
    // disconnected the moment the exchange completes, and onPortalResponse
    // ignores any reply whose sender path is not the step currently awaited,
    // so a late Response of a completed exchange can never finish a later
    // one.
    if (!QDBusConnection::sessionBus().connect(
            QLatin1String(kPortalService), requestPath,
            QLatin1String(kRequestInterface),
            QStringLiteral("Response"),
            this, SLOT(onPortalResponse(uint,QVariantMap)))) {
        failCurrentExchange(tr("The system screen picker could not be opened."));
        return;
    }
    m_portalRequestPaths.append(requestPath);
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

void ScreenShareSourcePicker::onPortalResponse(uint code, const QVariantMap &results)
{
    if (!m_portalExchangeActive)
        return; // no exchange in flight: every reply is stale by definition
    // While an exchange is in flight this slot's only live subscriptions
    // are its own steps (finishCurrentExchange drops all of them at
    // completion), and portal Requests answer strictly in call order, so a
    // reply can only ever belong to the awaited step of this exchange.
    const quint64 serial = m_exchangeSerial;
    if (code == 1) {
        // Portal semantics: 1 is the user cancelling the picker.
        failCurrentExchange(tr("The screen share source selection was cancelled."));
        return;
    }
    if (code != 0) {
        failCurrentExchange(tr("The system screen picker could not be opened."));
        return;
    }
    const QString token = QStringLiteral("wsnative87-%1").arg(serial);
    switch (m_portalStep) {
    case Step::CreateSession: {
        m_sessionPath = extractPortalString(results.value(QStringLiteral("session_target")));
        if (m_sessionPath.isEmpty()) {
            failCurrentExchange(tr("The system screen picker could not be opened."));
            return;
        }
        // Step 2, the source-selection contract proper: the types mask is
        // what makes screens (bit 0) and windows (bit 1) selectable inside
        // the portal dialog, so the picker yields a live screen or window
        // reference rather than a still image.
        m_portalStep = Step::SelectSources;
        callPortalStep(serial, "SelectSources", {QString(),
            QDBusObjectPath(m_sessionPath),
            QVariantMap{{QStringLiteral("handle_token"), token},
                {QStringLiteral("types"), QVariant::fromValue<quint32>(0x1 | 0x2)}}});
        return;
    }
    case Step::SelectSources:
        // Step 3: hand back the streams the user selected.
        m_portalStep = Step::PickStreams;
        callPortalStep(serial, "PickStreams", {QString(),
            QDBusObjectPath(m_sessionPath),
            QVariantMap{{QStringLiteral("handle_token"), token}}});
        return;
    case Step::PickStreams: {
        QString error;
        const QString streamReference = streamReferenceFromResult(results, error);
        if (streamReference.isEmpty()) {
            failCurrentExchange(error);
            return;
        }
        Source source;
        source.id = QStringLiteral("portal-stream");
        const QString name = extractPortalString(results.value(QStringLiteral("name")));
        source.name = name.isEmpty() ? tr("Screen capture") : name;
        // The live reference PickStreams answered with: the portal stream
        // object path, which a capture pipeline resolves to a PipeWire node
        // through org.freedesktop.portal.ScreenCast.OpenPipeWireRemote.
        source.uri = streamReference;
        source.fromPortal = true;
        source.live = true;
        finishCurrentExchange();
        emit sourceSelected(toMap(source));
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
    // Drop every Request subscription the exchange opened before anything
    // else can start (finding 3): after this point no reply of this exchange
    // can reach onPortalResponse at all.
    auto bus = QDBusConnection::sessionBus();
    for (const auto &requestPath : std::as_const(m_portalRequestPaths)) {
        bus.disconnect(QLatin1String(kPortalService), requestPath,
            QLatin1String(kRequestInterface), QStringLiteral("Response"),
            this, SLOT(onPortalResponse(uint,QVariantMap)));
    }
    m_portalRequestPaths.clear();
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

QString ScreenShareSourcePicker::streamReferenceFromResult(
    const QVariantMap &results, QString &error)
{
    error.clear();
    const QString reference = extractPortalString(results.value(QStringLiteral("streams")));
    if (reference.isEmpty())
        error = tr("The screen share result did not include a selectable stream.");
    return reference;
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
