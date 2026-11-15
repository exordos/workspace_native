#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>

#include <optional>

namespace workspace {

// OS power observer, the native counterpart of the reference power bridge
// (docs/NATIVE_DESKTOP_POWER_STATE.md). It watches the session bus for
// logind's PrepareForSleep (true = about to sleep, false = just resumed) and
// for UPower's OnBattery property plus its PropertiesChanged updates.
//
// Degrading is the whole point of keeping this separate: with no session bus
// nothing is subscribed, with a bus but no daemons the initial Get fails and
// is ignored, and every caller keeps its mains-power behaviour - exactly how
// the reference behaves in the browser build where no bridge exists.
class PowerWatcher final : public QObject
{
    Q_OBJECT
public:
    explicit PowerWatcher(QObject *parent = nullptr);

    // Subscribes on the given bus (the session bus by default) and reads the
    // initial battery state once. Calling it twice is a no-op.
    void start(const QDBusConnection &bus = QDBusConnection::sessionBus());

    bool isOnBattery() const { return m_onBattery; }

signals:
    void sleepAboutToStart();
    void resumed();
    void batteryChanged(bool onBattery);

private slots:
    void handlePrepareForSleep(const QDBusMessage &message);
    void handlePropertiesChanged(const QDBusMessage &message);

private:
    void setOnBattery(bool onBattery);

    std::optional<QDBusConnection> m_bus; // held so subscriptions keep their connection
    bool m_started = false;
    bool m_onBattery = false;
};

} // namespace workspace
