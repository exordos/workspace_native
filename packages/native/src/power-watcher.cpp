#include "power-watcher.h"

#include <QDBusArgument>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>

namespace workspace {
namespace {
const QString Login1Service = QStringLiteral("org.freedesktop.login1");
const QString Login1Path = QStringLiteral("/org/freedesktop/login1");
const QString Login1Manager = QStringLiteral("org.freedesktop.login1.Manager");
const QString UPowerService = QStringLiteral("org.freedesktop.UPower");
const QString UPowerPath = QStringLiteral("/org/freedesktop/UPower");
const QString UPowerInterface = QStringLiteral("org.freedesktop.UPower");
const QString PropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");
} // namespace

PowerWatcher::PowerWatcher(QObject *parent) : QObject(parent) {}

void PowerWatcher::start(const QDBusConnection &bus)
{
    if (m_started)
        return;
    m_started = true;
    if (!bus.isConnected())
        return; // no session bus at all: watch nothing, keep mains cadence
    m_bus = bus;
    QDBusConnection connection = bus; // connect() is a non-const member
    connection.connect(Login1Service, Login1Path, Login1Manager, QStringLiteral("PrepareForSleep"),
        this, SLOT(handlePrepareForSleep(QDBusMessage)));
    connection.connect(UPowerService, UPowerPath, PropertiesInterface, QStringLiteral("PropertiesChanged"),
        this, SLOT(handlePropertiesChanged(QDBusMessage)));
    // The initial read mirrors the reference power:getState best-effort read;
    // an error (no UPower, service unknown) simply leaves OnBattery false.
    auto call = bus.asyncCall(QDBusMessage::createMethodCall(UPowerService, UPowerPath,
        PropertiesInterface, QStringLiteral("Get"))
        << QVariant(UPowerInterface) << QStringLiteral("OnBattery"));
    auto *pending = new QDBusPendingCallWatcher(call, this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watched) {
        watched->deleteLater();
        if (watched->isError())
            return;
        const auto arguments = watched->reply().arguments();
        if (arguments.isEmpty())
            return;
        // Properties.Get wraps every value in a variant.
        const auto wrapped = arguments.first().value<QDBusVariant>().variant();
        if (wrapped.userType() == QMetaType::Bool)
            setOnBattery(wrapped.toBool());
    });
}

void PowerWatcher::handlePrepareForSleep(const QDBusMessage &message)
{
    const auto arguments = message.arguments();
    if (arguments.isEmpty() || arguments.first().userType() != QMetaType::Bool)
        return;
    if (arguments.first().toBool())
        emit sleepAboutToStart();
    else
        emit resumed();
}

void PowerWatcher::handlePropertiesChanged(const QDBusMessage &message)
{
    const auto arguments = message.arguments();
    if (arguments.size() < 2 || arguments.first().toString() != UPowerInterface)
        return;
    // a{sv} arrives either decoded or as a raw D-Bus argument depending on
    // how the emitter sent it; accept both shapes.
    QVariantMap changed;
    const auto holder = arguments.at(1);
    if (holder.userType() == QMetaType::QVariantMap)
        changed = holder.toMap();
    else if (holder.canConvert<QDBusArgument>()) {
        const auto argument = holder.value<QDBusArgument>();
        if (argument.currentSignature() == QLatin1String("a{sv}"))
            argument >> changed;
    }
    const auto updated = changed.constFind(QStringLiteral("OnBattery"));
    if (updated == changed.constEnd() || updated.value().userType() != QMetaType::Bool)
        return;
    setOnBattery(updated.value().toBool());
}

void PowerWatcher::setOnBattery(bool onBattery)
{
    if (m_onBattery == onBattery)
        return;
    m_onBattery = onBattery;
    emit batteryChanged(onBattery);
}

} // namespace workspace
