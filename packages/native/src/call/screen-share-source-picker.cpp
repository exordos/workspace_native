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
    const auto bus = QDBusConnection::sessionBus();
    QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(kPortalService),
        QLatin1String(kPortalPath),
        QStringLiteral("org.freedesktop.portal.Screenshot"),
        QStringLiteral("PictureUri"));
    // parent_window stays empty (no toplevel identity to advertise yet) and
    // handle_token is an opaque correlation id, as the portal interface asks.
    call.setArguments({QString(), QVariantMap{{QStringLiteral("handle_token"),
        QStringLiteral("wsnative87")}}});
    // QDBusMessage has no per-message timeout: the deadline rides the
    // connection call itself. The exchange stays asynchronous so the calling
    // thread never spins a nested event loop.
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, m_portalTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher](QDBusPendingCallWatcher *self) {
        watcher->deleteLater();
        if (!m_portalExchangeActive)
            return;
        if (self->isError()) {
            m_portalExchangeActive = false;
            emit sourceFailed(tr("The system screen picker could not be opened."));
            return;
        }
        const QDBusMessage reply = self->reply();
        QString requestPath;
        if (!reply.arguments().isEmpty())
            requestPath = reply.arguments().first().value<QDBusObjectPath>().path();
        if (requestPath.isEmpty()) {
            m_portalExchangeActive = false;
            emit sourceFailed(tr("The system screen picker could not be opened."));
            return;
        }
        const bool subscribed = QDBusConnection::sessionBus().connect(
            QLatin1String(kPortalService), requestPath,
            QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"), this,
            SLOT(onPortalResponse(uint,QVariantMap)));
        if (!subscribed) {
            m_portalExchangeActive = false;
            emit sourceFailed(tr("The system screen picker could not be opened."));
            return;
        }
        QTimer::singleShot(m_portalTimeoutMs, this, [this] {
            if (!m_portalExchangeActive)
                return;
            m_portalExchangeActive = false;
            emit sourceFailed(tr("The screen share source selection timed out."));
        });
    });
}

void ScreenShareSourcePicker::onPortalResponse(uint code, const QVariantMap &results)
{
    finishDefaultPortal(quint32(code), results);
}

void ScreenShareSourcePicker::finishDefaultPortal(quint32 code, const QVariantMap &results)
{
    if (!m_portalExchangeActive)
        return;
    m_portalExchangeActive = false;
    if (code != 0) {
        // Portal semantics: 1 is the user cancelling the picker.
        emit sourceFailed(code == 1
            ? tr("The screen share source selection was cancelled.")
            : tr("The system screen picker could not be opened."));
        return;
    }
    QString error;
    const QString uri = pictureUriFromResult(results, error);
    if (uri.isEmpty()) {
        emit sourceFailed(error);
        return;
    }
    Source source;
    source.id = QStringLiteral("portal-screenshot");
    source.name = tr("Screen capture");
    source.uri = uri;
    source.fromPortal = true;
    emit sourceSelected(toMap(source));
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
        sources.append(source);
    }
    return sources;
}

QString ScreenShareSourcePicker::pictureUriFromResult(const QVariantMap &results, QString &error)
{
    error.clear();
    const QString uri = extractPortalString(results.value(QStringLiteral("PictureUri")));
    if (uri.isEmpty())
        error = tr("The screen share result did not include a picture.");
    return uri;
}

QVariantMap ScreenShareSourcePicker::toMap(const Source &source)
{
    return QVariantMap{
        {QStringLiteral("id"), source.id},
        {QStringLiteral("name"), source.name},
        {QStringLiteral("uri"), source.uri},
        {QStringLiteral("fromPortal"), source.fromPortal},
    };
}

} // namespace workspace
