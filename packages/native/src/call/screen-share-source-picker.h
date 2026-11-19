#pragma once

#include <QObject>
#include <QVariantMap>

#include <functional>

class QTimer;

namespace workspace {

// #87: the native share-source picker for embedded Jitsi calls, the platform
// analogue of the Electron source picker the reference SDK owns
// (workspace_ui 61dacef7 packages/electron/src/main.ts:364-367 installs it at
// window creation). Since round 2 (finding 4) the default transport is the
// portal's LIVE source-selection contract,
// org.freedesktop.portal.ScreenCast on the session bus (service
// org.freedesktop.portal.Desktop, object /org/freedesktop/portal/desktop):
// CreateSession answers a session target, SelectSources (types mask
// screens|windows) lets the user pick a screen or window, and PickStreams
// answers "streams" - stream-reference object paths that a capture pipeline
// consumes via OpenPipeWireRemote, not a still PictureUri screenshot file,
// which is what the replaced transport returned. When no portal is registered
// on the session bus the picker falls back deterministically to Qt's own
// screen model.
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

    // True when org.freedesktop.portal.Desktop answers on the session bus.
    bool portalAvailable() const;

    // Portal path when available, deterministic Qt screen-model fallback
    // otherwise. Exactly one of sourceSelected/sourceFailed follows each
    // call (the default portal transport is asynchronous until it answers).
    // A call while an exchange is still in flight is ignored; a completed
    // exchange leaves no timer or subscription behind.
    Q_INVOKABLE void requestSource();

    // One fallback source per Qt screen, in Qt's own screen order.
    static QList<Source> screenModelSources();

    // The first stream reference out of PickStreams' a{sv} results: "streams"
    // is an array of stream-reference strings (ao object paths on a real
    // portal, delivered through the reply variants as a QDBusArgument stream,
    // QStringList, or plain string forms). Empty plus an error text when
    // absent. Public for the same reason the old PictureUri reader was: the
    // result forms are only observable over a real portal otherwise.
    static QString streamReferenceFromResult(const QVariantMap &results, QString &error);

signals:
    void sourceSelected(QVariantMap source);
    void sourceFailed(QString reason);

private slots:
    // Receives the portal Request Response(u code, a{sv} results) signal of
    // the in-flight exchange. QDBusConnection subscriptions bind to a
    // receiver slot (not a functor), so exchange correlation is carried by
    // the subscription set itself: while an exchange is in flight this slot's
    // only live subscriptions are its own steps (finishCurrentExchange drops
    // every one of them at completion, and the steps answer strictly in
    // call order), so a late reply of a completed exchange can never reach a
    // later exchange (finding 3).
    void onPortalResponse(uint code, const QVariantMap &results);

private:
    enum class Step { CreateSession, SelectSources, PickStreams };

    bool defaultPortalProbe() const;
    void startDefaultPortalExchange();
    void callPortalStep(quint64 serial, const char *method, const QVariantList &args);
    void subscribePortalRequest(quint64 serial, const QString &requestPath);
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
    QStringList m_portalRequestPaths; // Request subscriptions owned by the exchange
};

} // namespace workspace
