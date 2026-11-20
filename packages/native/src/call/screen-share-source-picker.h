#pragma once

#include <QHash>
#include <QObject>
#include <QVariantMap>

#include <functional>
#include <utility>

class QDBusMessage;
class QTimer;

namespace workspace {

// #87: the native share-source picker for embedded Jitsi calls, the platform
// analogue of the Electron source picker the reference SDK owns
// (workspace_ui 61dacef7 packages/electron/src/main.ts:364-367 installs it at
// window creation). Since round 2 (finding 4) the default transport is the
// portal's LIVE source-selection contract,
// org.freedesktop.portal.ScreenCast on the session bus (service
// org.freedesktop.portal.Desktop, object /org/freedesktop/portal/desktop):
// CreateSession creates a session, SelectSources configures it, and Start
// opens the consent dialog. The selected PipeWire node and session handle
// identify the source for OpenPipeWireRemote. With no portal the picker
// falls back to Qt's screen model.
//
// Each exchange is tracked by its own serial and owns its own timeout timer
// and Request subscription (round 2, finding 3): completion cancels both
// first, so a stale timer or a late Response can never fail or fake a later
// exchange.
//
// Qt 6.10.2 gives the application the capture grant decision but no public
// API to choose which screen or window the granted getDisplayMedia session
// captures, so this class publishes the picked live source (sourceSelected)
// as the contract a future capture pipeline consumes instead of silently
// inventing an untested integration. See
// docs/NATIVE_DESKTOP_CALL_DISPLAY_CAPTURE.md.
class ScreenShareSourcePicker final : public QObject
{
    Q_OBJECT
public:
    struct Source
    {
        QString id;
        QString name;
        QString uri;
        bool fromPortal = false;
        bool live = false;
    };

    // Injected seams (the call surface's established std::function pattern):
    // the probe replaces the portal-availability check and the request
    // replaces the whole portal exchange, so both portal paths and the
    // fallback are exercisable on a display with no portal daemon.
    using PortalProbe = std::function<bool()>;
    using PortalRequest = std::function<bool(Source &source, QString &error)>;
    void setPortalProbe(PortalProbe probe);
    void setPortalRequest(PortalRequest request);
    // Upper bound for one whole portal exchange (all its steps and the pick).
    void setPortalTimeoutMs(int ms);

    explicit ScreenShareSourcePicker(QObject *parent = nullptr);
    ~ScreenShareSourcePicker() override;

    // True when org.freedesktop.portal.Desktop answers on the session bus.
    bool portalAvailable() const;

    // Portal path when available, deterministic Qt screen-model fallback
    // otherwise. Exactly one of sourceSelected/sourceFailed follows each
    // call (the default portal transport is asynchronous until it answers).
    // A call while an exchange is still in flight is ignored; a completed
    // exchange leaves no timer or subscription behind.
    Q_INVOKABLE void requestSource();
    Q_INVOKABLE void releaseSource();

    // One fallback source per Qt screen, in Qt's own screen order.
    static QList<Source> screenModelSources();

    // Decode the single selected (PipeWire node ID, properties) tuple.
    static QVariantMap streamFromResult(const QVariantMap &results);

signals:
    void sourceSelected(QVariantMap source);
    void sourceFailed(QString reason);

private slots:
    void onPortalResponse(uint code, const QVariantMap &results, const QDBusMessage &signal);

private:
    enum class Step { CreateSession, SelectSources, Start };

    bool defaultPortalProbe() const;
    void startDefaultPortalExchange();
    void callPortalStep(quint64 serial, const char *method, const QVariantList &args);
    bool subscribePortalRequest(const QString &requestPath);
    void unsubscribePortalRequest();
    void handlePortalResult(uint code, const QVariantMap &results);
    void armPortalTimeout(quint64 serial);
    bool isCurrentExchange(quint64 serial) const;
    void failCurrentExchange(const QString &reason);
    void finishCurrentExchange();
    static QVariantMap toMap(const Source &source);

    PortalProbe m_portalProbe;
    PortalRequest m_portalRequest;
    int m_portalTimeoutMs = 30000;
    bool m_portalExchangeActive = false;
    quint64 m_exchangeSerial = 0;
    Step m_portalStep = Step::CreateSession;
    QString m_sessionPath;
    QTimer *m_portalTimer = nullptr;
    QString m_requestPath;
    bool m_waitingForRequestPath = false;
    QHash<QString, std::pair<uint, QVariantMap>> m_earlyResponses;
    QString m_selectedSession;
};

} // namespace workspace
