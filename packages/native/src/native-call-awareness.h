#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>

#include <optional>

namespace workspace {

// Shell-side call awareness (issue #86, docs/NATIVE_DESKTOP_CALL_AWARENESS.md):
// while a call is active the shell keeps the display awake and the tray
// tooltip shows the room. This is the native counterpart of the reference
// Electron IPC handlers "call:start"/"call:update"/"call:end"
// (workspace_ui 61dacef7 packages/electron/src/main.ts:1268-1322). The call
// UI itself does not exist in the native client yet, so this service is
// dormant: the Q_INVOKABLE API is the seam the future call surface calls.
//
// The class owns the call state machine and the freedesktop display-sleep
// cookie; the tooltip text itself is composed by NativeDesktop (it owns the
// app name and the locale/unread refresh that must keep the call suffix).
class CallAwareness final : public QObject
{
    Q_OBJECT
public:
    explicit CallAwareness(QObject *parent = nullptr);
    ~CallAwareness() override;

    // ScreenSaver and PowerManagement.Inhibit are session services, so a
    // start() with no argument selects the session bus; the injectable
    // parameter keeps the private fixture bus working in tests (the
    // PowerWatcher seam pattern, issue #78).
    static QDBusConnection defaultBus() { return QDBusConnection::sessionBus(); }
    void start(const QDBusConnection &bus = defaultBus());

    // Reference rule: strip control chars [\x00-\x1f\x7f], cap at 128 chars,
    // empty falls back to "Call". The reference does not trim whitespace.
    static QString sanitizeRoomName(const QString &raw);

    bool callActive() const { return !m_room.isEmpty(); }
    QString activeRoom() const { return m_room; }
    // 0 while no inhibit cookie is held. The cookie arrives asynchronously:
    // tests and callers must wait for it (QTRY) after callStarted.
    quint32 inhibitCookie() const { return m_cookie.value_or(0); }

    // Which freedesktop service handed out the held cookie.
    enum class ServiceKind { None, ScreenSaver, PowerManagement };

    Q_INVOKABLE void callStarted(const QString &room, int participants);
    Q_INVOKABLE void callParticipantsUpdated(int count);
    Q_INVOKABLE void callEnded();

signals:
    // The compositor of the tray tooltip listens here. room is sanitized and
    // empty when idle; participants is the count to render, or -1 when no
    // count is published yet (reference start parity: the count only ever
    // arrives through callParticipantsUpdated).
    void callStateChanged(const QString &room, int participants);

private:
    void acquireInhibit();
    void requestInhibit(int generation, ServiceKind kind);
    void releaseInhibit();
    void sendUninhibit(ServiceKind kind, quint32 cookie);

    QDBusConnection m_bus {QDBusConnection::sessionBus()};
    bool m_started = false;
    QString m_room;
    int m_participants = -1;
    std::optional<quint32> m_cookie;
    ServiceKind m_inhibitService = ServiceKind::None;
    // Identifies the one Inhibit request whose reply is still awaited
    // (generation + service), -1/None while no reply is pending. Tracked per
    // generation (issue #86 review): a reply left in flight by an already
    // ended call never blocks a new call from acquiring its own inhibitor,
    // and a reply that lands after the deadline is recognisably late.
    int m_inhibitPendingGeneration = -1;
    ServiceKind m_inhibitPendingKind = ServiceKind::None;
    int m_inhibitGeneration = 0;
};

} // namespace workspace
