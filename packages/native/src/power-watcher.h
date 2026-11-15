#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>

#include <optional>

namespace workspace {

// OS power observer, the native counterpart of the reference power bridge
// (docs/NATIVE_DESKTOP_POWER_STATE.md). It watches the system bus for
// logind's PrepareForSleep (true = about to sleep, false = just resumed) and
// for UPower's OnBattery property plus its PropertiesChanged updates. Both
// services publish on the system bus and dbus policy restricts ownership of
// their well-known names to the daemons themselves, so a subscription on the
// system bus observes the trusted services.
//
// Degrading is the whole point of keeping this separate: with no system bus
// nothing is subscribed, with a bus but no daemons the initial Get fails and
// is ignored, and every caller keeps its mains-power behaviour - exactly how
// the reference behaves in the browser build where no bridge exists.
class PowerWatcher final : public QObject
{
    Q_OBJECT
public:
    explicit PowerWatcher(QObject *parent = nullptr);

    // Which bus a start() with no argument selects: logind and UPower are
    // system services, so the app starts the watcher on the system bus; the
    // injectable start() parameter keeps the private fixture bus working in
    // tests. Exposed as a seam so a test can pin the selection without
    // touching host buses (fix-round-1 finding 1, cid 4576526925).
    enum class Bus { Session, System };
    static constexpr Bus defaultBusKind() { return Bus::System; }
    static QDBusConnection defaultBus()
    {
        return defaultBusKind() == Bus::System ? QDBusConnection::systemBus()
                                               : QDBusConnection::sessionBus();
    }

    // Subscribes on the given bus (the system bus by default) and reads the
    // initial battery state once. Calling it twice is a no-op.
    void start(const QDBusConnection &bus = defaultBus());

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
    // Monotonic battery-state stamp (fix-round-1 finding 2, cid 4576526932).
    // A PropertiesChanged update bumps it; the in-flight initial Get captured
    // it when the call was sent, and drops its reply once the stamp moved -
    // a late stale reply may never overwrite fresher signal state.
    quint64 m_batteryRevision = 0;
};

} // namespace workspace
