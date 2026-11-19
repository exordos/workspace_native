#pragma once

#include <QObject>
#include <QVariantMap>

#include <functional>

namespace workspace {

// #87: the native share-source picker for embedded Jitsi calls, the platform
// analogue of the Electron source picker the reference SDK owns
// (workspace_ui 61dacef7 packages/electron/src/main.ts:364-367 installs it at
// window creation). The portal transport is
// org.freedesktop.portal.Screenshot.PictureUri on the session bus
// (service org.freedesktop.portal.Desktop, object
// /org/freedesktop/portal/desktop); when no portal is registered the picker
// falls back deterministically to Qt's own screen model.
//
// Qt 6.10.2 gives the application the capture grant decision but no public
// API to choose which screen or window the granted getDisplayMedia session
// captures, so this class publishes the picked source (sourceSelected) as
// the contract a future capture pipeline consumes instead of silently
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
    };

    // Injected seams (the call surface's established std::function pattern):
    // the probe replaces the portal-availability check and the request
    // replaces the whole portal exchange, so both portal paths and the
    // fallback are exercisable on a display with no portal daemon.
    using PortalProbe = std::function<bool()>;
    using PortalRequest = std::function<bool(Source &source, QString &error)>;
    void setPortalProbe(PortalProbe probe);
    void setPortalRequest(PortalRequest request);
    // Upper bound for the portal reply and the user's pick.
    void setPortalTimeoutMs(int ms);

    explicit ScreenShareSourcePicker(QObject *parent = nullptr);

    // True when org.freedesktop.portal.Desktop answers on the session bus.
    bool portalAvailable() const;

    // Portal path when available, deterministic Qt screen-model fallback
    // otherwise. Exactly one of sourceSelected/sourceFailed follows each
    // call (the default portal transport is asynchronous until it answers).
    Q_INVOKABLE void requestSource();

    // One fallback source per Qt screen, in Qt's own screen order.
    static QList<Source> screenModelSources();

    // Unwraps one PictureUri member out of the portal Response a{sv} results:
    // values that arrive inside a QVariant come back wrapped in QDBusVariant,
    // and list members arrive as a QDBusArgument stream (beginArray/atEnd,
    // strings or object paths). Empty plus an error text when absent.
    static QString pictureUriFromResult(const QVariantMap &results, QString &error);

signals:
    void sourceSelected(QVariantMap source);
    void sourceFailed(QString reason);

private slots:
    // Receives the portal Request Response(u code, a{sv} results) signal.
    void onPortalResponse(uint code, const QVariantMap &results);

private:
    bool defaultPortalProbe() const;
    void startDefaultPortalExchange();
    void finishDefaultPortal(quint32 code, const QVariantMap &results);
    static QVariantMap toMap(const Source &source);

    PortalProbe m_portalProbe;
    PortalRequest m_portalRequest;
    int m_portalTimeoutMs = 30000;
    bool m_portalExchangeActive = false;
};

} // namespace workspace
