#include "api-client.h"
#ifdef Q_OS_LINUX
#include "live-acceptance-trace.h"
#include <QQuickWindow>
#endif
#include "avatar-images.h"
#include "message-images.h"
#include "file-client.h"
#include "native-app.h"
#include "native-call-awareness.h"
#include "native-controller.h"
#include "native-desktop.h"
#include "native-cache-service.h"
#include "native-drafts.h"
#include "native-deeplink.h"
#include "native-instance.h"
#include "native-logging.h"
#include "native-updater.h"
#include "native-updater-taskbar.h"
#include "native-preferences.h"
#include "native-realtime-cursor-store.h"
#include "realtime-client.h"
#include "power-watcher.h"
#include "session.h"
#include "session-vault.h"

#include <QApplication>
#include <QEvent>
#include <QIcon>
#include <QResource>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>
#include <QWindow>
#include <openssl/ssl.h>
#include <limits>

namespace {

// Electron forwards focus/blur to the renderer as window:activity and stops
// the taskbar flash on focus (main.ts:435-437); the native client mirrors the
// same activation changes onto the controller for notification gating.
class WindowFocusWatcher final : public QObject {
public:
    explicit WindowFocusWatcher(workspace::NativeController *target) : QObject(target), m_target(target) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *window = qobject_cast<QWindow *>(watched);
        if (window && event->type() == QEvent::ActivationChange)
            m_target->setWindowFocused(window->isActive());
        return false;
    }

private:
    workspace::NativeController *m_target;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication::setOrganizationName("Exordos");
    QCoreApplication::setApplicationName("Workspace Native Preview");
    QCoreApplication::setApplicationVersion(WORKSPACE_NATIVE_VERSION);
    QGuiApplication::setDesktopFileName("com.exordos.workspace.native.preview");
    QApplication app(argc, argv);
    // Source parity: the last argument that validates as an external deep link
    // wins on cold start, mirroring the Electron startup argv scan.
    QString startupDeepLink;
    for (int i = 1; i < argc; ++i) {
        const auto argument = QString::fromLocal8Bit(argv[i]);
        if (workspace::parseExternalDeepLink(argument).has_value())
            startupDeepLink = argument;
    }
    // A direct SSL dependency keeps Qt's TLS backend and message crypto on the
    // bundled OpenSSL pair before transitive system libraries can load another.
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
        qCritical("The encryption runtime could not be initialized.");
        return 1;
    }
    app.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle("Basic");
    // Diagnostics log file (issue #77): every qInfo/qWarning line is also
    // appended to workspace_logs/workspace.log with the reference rotation.
    workspace::NativeLogging diagnosticsLog;
    workspace::installNativeLogHandler(diagnosticsLog);
    const auto removeLogHandler = qScopeGuard(&workspace::removeNativeLogHandler);
    workspace::NativeInstance instance;
    const auto outcome = instance.acquireOrActivate(startupDeepLink);
    if (outcome == workspace::NativeInstance::Outcome::Secondary) return 0;
    if (outcome == workspace::NativeInstance::Outcome::Failed) {
        qCritical("%s", qPrintable(instance.error()));
        return 1;
    }
    Q_INIT_RESOURCE(native_app_icon);
    QGuiApplication::setWindowIcon(QIcon(":/workspace/brand/icon.png"));
#ifdef Q_OS_LINUX
    workspace::configureLiveAcceptanceNetwork();
#endif
    workspace::ApiClient api;
    workspace::SessionVault vault;
    workspace::Session session(&api);
    session.setVault(&vault);
    workspace::NativeChatKeyVault chatKeys;
    workspace::FileClient files(&api);
    workspace::AvatarImages avatars(&api, &session, &files);
    workspace::MessageImages messageImages(&api, &session, &files);
    workspace::NativePreferences preferences;
    workspace::NativeCacheService cache;
    workspace::NativeDraftService draftStorage;
    workspace::NativeDrafts drafts(&draftStorage);
    workspace::bindNativeCachePreferences(cache, preferences);
    workspace::NativeController controller(&api, &session);
    // The login wizard persists committed organization origins (issue #58).
    controller.setPreferences(&preferences);
#if defined(Q_OS_MACOS)
    const auto deviceName = QStringLiteral("Mac — Desktop");
#elif defined(Q_OS_WIN)
    const auto deviceName = QStringLiteral("Windows — Desktop");
#else
    const auto deviceName = QStringLiteral("Linux — Desktop");
#endif
    workspace::bindNativeChatKeys(chatKeys, session, api, controller, deviceName);
    controller.useCache(&cache);
    controller.useDrafts(&drafts);
    controller.useFiles(&files);
    workspace::RealtimeCursorStore realtimeCursors;
    workspace::RealtimeClient realtime(&api, &session);
    realtime.useCursorStore(&realtimeCursors);
    controller.useRealtime(&realtime);
    // System-bus power observer: resume reconnects realtime at once, and a
    // battery source widens the presence heartbeat (issue #78). With no
    // logind/UPower on the bus both paths silently stay at mains behaviour.
    workspace::PowerWatcher powerWatcher;
    controller.setPowerWatcher(&powerWatcher);
    powerWatcher.start();
    workspace::CallAwareness callAwareness;
    callAwareness.start(*controller.callWindow());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("callAwareness", &callAwareness);
    workspace::NativeUpdater updater;
    workspace::loadNativeUi(engine, &controller, &preferences, &avatars, &updater, &messageImages);
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QWindow *>(engine.rootObjects().first());
#ifdef Q_OS_LINUX
    workspace::observeLiveAcceptance(qobject_cast<QQuickWindow *>(window), api, controller, cache, realtime);
#endif
    workspace::NativeDesktop desktop(window);
    // The tray tooltip follows the call state (issue #86): the room with an
    // optional participant count while active, the bare app name when idle.
    QObject::connect(&callAwareness, &workspace::CallAwareness::callStateChanged,
        &desktop, &workspace::NativeDesktop::setCallState);
    // Mirror updater download progress into the window's taskbar progress
    // hint (issue #76): percent/100 while downloading, cleared on downloaded
    // and on error, reproducing the reference setProgressBar semantics. Bare
    // window managers ignore the hint (docs/NATIVE_DESKTOP_UPDATER_TASKBAR_PROGRESS.md).
    workspace::UpdaterTaskbarProgress updaterTaskbar;
    updaterTaskbar.attach(window, &updater);
    // Desktop notifications over the session bus with X11 attention support.
    auto *notifications = controller.notifications();
    auto *desktopNotifications = new workspace::DBusNotificationTransport(
        [window] { return window ? window->winId() : quintptr{}; }, &controller);
    desktopNotifications->setClickCallback(
        [notifications](const QString &tag) { notifications->click(tag); });
    notifications->setTransport(desktopNotifications);
    if (window) {
        auto *focusWatcher = new WindowFocusWatcher(&controller);
        window->installEventFilter(focusWatcher);
        controller.setWindowFocused(window->isActive());
        // hide/show/minimize map onto the reference hidden WindowActivityState
        // for the presence cadence (issue #78); Qt delivers it as the
        // visibilityChanged signal rather than a filterable event type. Qt
        // keeps isVisible() true while minimized, so the minimized tier is
        // projected separately - the reference ladder treats minimized as
        // hidden (fix-round-2, cid 4576526934).
        const auto projectVisibility = [&controller, window](QWindow::Visibility visibility) {
            const bool minimized = visibility == QWindow::Minimized;
            controller.setWindowMinimized(minimized);
            controller.setWindowVisible(!minimized && visibility != QWindow::Hidden);
        };
        projectVisibility(window->visibility());
        QObject::connect(window, &QWindow::visibilityChanged, &controller, projectVisibility);
    }
    QString restartExecutable;
    bool shuttingDown = false;
    const auto beginShutdown = [&cache, &chatKeys, &controller, &app, &drafts, &draftStorage,
                                &shuttingDown, &restartExecutable, window] {
        if (shuttingDown) return;
        shuttingDown = true;
        window->hide();
        workspace::shutdownNativeStorage(drafts, draftStorage, cache, &app,
                         [&chatKeys, &controller, &app, &shuttingDown, &restartExecutable, window](bool okay) {
            if (!okay) {
                shuttingDown = false; restartExecutable.clear();
                controller.reportDraftFlushFailure();
                window->show(); window->requestActivate();
                return;
            }
            controller.setChatKeyStoreOpening(false);
            chatKeys.close();
            app.quit();
        });
    };
    QObject::connect(&app, &QGuiApplication::lastWindowClosed, &cache, beginShutdown);
    QObject::connect(&desktop, &workspace::NativeDesktop::quitRequested, &cache, beginShutdown);
    QObject::connect(&updater, &workspace::NativeUpdater::restartRequested, &cache,
                     [&restartExecutable, &shuttingDown, beginShutdown](const QString &executable) {
        if (shuttingDown) return;
        restartExecutable = executable;
        beginShutdown();
    });
    QObject::connect(&desktop, &workspace::NativeDesktop::checkForUpdatesRequested, &updater,
                     [&updater, &shuttingDown, window] {
        if (shuttingDown) return;
        window->setProperty("updatesVisible", true);
        window->show();
        window->requestActivate();
        updater.check();
    });
#ifdef Q_OS_LINUX
    QTimer::singleShot(10000, &updater, &workspace::NativeUpdater::check);
    QTimer updateChecks;
    updateChecks.setInterval(4 * 60 * 60 * 1000);
    QObject::connect(&updateChecks, &QTimer::timeout, &updater, &workspace::NativeUpdater::check);
    updateChecks.start();
#endif
    preferences.attachWindow(window);
    const auto activate = [&preferences, &shuttingDown] { if (!shuttingDown) preferences.activateWindow(); };
    QObject::connect(&desktop, &workspace::NativeDesktop::activateRequested, &app, activate);
    QObject::connect(&desktop, &workspace::NativeDesktop::sectionRequested, &app,
                     [&](const QString &section) {
                         if (shuttingDown) return;
                         activate();
                         controller.showSection(section);
                     });
    // A valid notification click reuses this same guarded activation path as
    // tray activation and second-instance deep links (issue #117); expired or
    // unknown tags emit nothing and keep the window untouched.
    QObject::connect(notifications, &workspace::NotificationService::activationRequested,
                     &app, activate);
    QObject::connect(&preferences, &workspace::NativePreferences::localeChanged,
                     &desktop, &workspace::NativeDesktop::refreshLabels);
    QString badgeOwner;
    const auto updateUnread = [&controller, &desktop, &badgeOwner] {
        badgeOwner = controller.ownerKey();
        qint64 count = 0;
        if (controller.authenticated()) {
            for (const auto &value : controller.streams()) {
                const auto unread = qMax<qint64>(0, value.toMap().value("unread").toLongLong());
                count += qMin(unread, (std::numeric_limits<qint64>::max)() - count);
            }
        }
        desktop.setUnreadCount(count);
    };
    QObject::connect(&controller, &workspace::NativeController::streamsChanged, &desktop, updateUnread);
    QObject::connect(&controller, &workspace::NativeController::stateChanged, &desktop,
                     [&controller, &badgeOwner, updateUnread] {
        if (controller.ownerKey() != badgeOwner) updateUnread();
    });
    updateUnread();
    QObject::connect(&instance, &workspace::NativeInstance::activationRequested, &app,
        [&controller, activate](const QString &deepLink) {
            activate();
            if (!deepLink.isEmpty()) controller.openMessageLink(deepLink);
        });
    if (instance.activationPending()) activate();
    if (!startupDeepLink.isEmpty()) controller.openMessageLink(startupDeepLink);
    QTimer::singleShot(0, &session, &workspace::Session::resumeCurrentAccount);
    const int code = app.exec();
    if (!restartExecutable.isEmpty()) {
        // A successor must acquire the primary lock after cache/key shutdown.
        instance.release();
        if (!QProcess::startDetached(restartExecutable, {})) {
            qCritical("The updated application could not be started.");
            return 1;
        }
    }
    return code;
}
