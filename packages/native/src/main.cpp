#include <QSoundEffect>
#include "api-client.h"
#include "support/support-report.h"
#include <QFile>
#include <QFileInfo>
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
#include "power-watcher.h"
#include "session.h"
#include "session-vault.h"
#include "workspace-sessions.h"

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
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shobjidl.h>
#endif

namespace {

// Electron forwards focus/blur to the renderer as window:activity and stops
// the taskbar flash on focus (main.ts:435-437); the native client mirrors the
// same activation changes onto the controller for notification gating.
class WindowFocusWatcher final : public QObject {
public:
    WindowFocusWatcher(workspace::NativeController *target, workspace::WorkspaceSessions *workspaces)
        : QObject(target), m_target(target), m_workspaces(workspaces) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *window = qobject_cast<QWindow *>(watched);
        if (window && event->type() == QEvent::ActivationChange) {
            m_target->setWindowFocused(window->isActive());
            m_workspaces->setWindowFocused(window->isActive());
        }
        return false;
    }

private:
    workspace::NativeController *m_target;
    workspace::WorkspaceSessions *m_workspaces;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication::setOrganizationName("Exordos");
    QCoreApplication::setApplicationName("Workspace Native Preview");
    QCoreApplication::setApplicationVersion(WORKSPACE_NATIVE_VERSION);
    if (argc == 2 && QByteArrayView(argv[1]) == "--support-report") {
        QCoreApplication app(argc, argv);
        QFile output;
        const auto report = workspace::SupportReport::basicReport();
        return !report.isEmpty() && output.open(stdout, QIODevice::WriteOnly)
            && output.write(report) == report.size() && output.flush() ? 0 : 1;
    }
    QGuiApplication::setDesktopFileName("com.exordos.workspace.native.preview");
#ifdef Q_OS_WIN
    if (FAILED(SetCurrentProcessExplicitAppUserModelID(QGuiApplication::desktopFileName().toStdWString().c_str())))
        qWarning("Could not set the Windows application identity.");
#endif
    QApplication app(argc, argv);
#ifdef Q_OS_LINUX
    // Packaged call runtime (#728): the package ships QtWebEngineProcess and
    // the Chromium .pak trees inside the prefix; point WebEngine at them when
    // the bundle marker exists. Development trees keep the system Qt paths.
    {
        const auto bundleRoot = QFileInfo(QCoreApplication::applicationDirPath() + "/..").canonicalFilePath();
        const auto webengineData = bundleRoot + "/share/qt-webengine";
        if (QFile::exists(webengineData + "/resources/qtwebengine_resources.pak")) {
            qputenv("QTWEBENGINEPROCESS_PATH", (bundleRoot + "/libexec/QtWebEngineProcess").toLocal8Bit());
            qputenv("QTWEBENGINE_RESOURCES_PATH", (webengineData + "/resources").toLocal8Bit());
            qputenv("QTWEBENGINE_LOCALES_PATH", (webengineData + "/locales").toLocal8Bit());
        }
    }
#endif
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
#ifdef Q_OS_MACOS
    app.installEventFilter(&instance);
#endif
    const auto earlyActivation = QObject::connect(&instance, &workspace::NativeInstance::activationRequested,
        &app, [&startupDeepLink](const QString &url) { if (!url.isEmpty()) startupDeepLink = url; });
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
    // Inline previews persist their encoded originals as derived cache data,
    // so a chat switch or restart re-renders from disk without a transfer (#729).
    messageImages.setCacheService(&cache);
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
    // System-bus power observer: resume reconnects realtime at once, and a
    // battery source widens the presence heartbeat (issue #78). With no
    // logind/UPower on the bus both paths silently stay at mains behaviour.
    workspace::PowerWatcher powerWatcher;
    controller.setPowerWatcher(&powerWatcher);
    powerWatcher.start();
    workspace::WorkspaceSessions workspaces(session, controller, vault, realtimeCursors);
    workspaces.setPowerWatcher(&powerWatcher);
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
    workspace::observeLiveAcceptance(qobject_cast<QQuickWindow *>(window), api, controller, cache, workspaces);
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
    auto *desktopNotifications = new workspace::DBusNotificationTransport(
        [window] { return window ? window->winId() : quintptr{}; }, &controller);
    desktopNotifications->setClickCallback(
        [&workspaces](const QString &tag) { workspaces.clickNotification(tag); });
    workspaces.setNotificationTransport(desktopNotifications);
    QObject::connect(&workspaces, &workspace::WorkspaceSessions::soundRequested,
        &preferences, &workspace::NativePreferences::previewNotificationSound);
    QHash<QString, QSoundEffect *> sounds;
    QObject::connect(&preferences, &workspace::NativePreferences::notificationSoundRequested, &app,
        [&sounds, &app](const QString &preset) {
            auto *&sound = sounds[preset];
            if (!sound) {
                sound = new QSoundEffect(&app);
                sound->setSource(QUrl(QStringLiteral("qrc:/workspace/sounds/") + preset + ".wav"));
            }
            sound->play();
        });
    if (window) {
        auto *focusWatcher = new WindowFocusWatcher(&controller, &workspaces);
        window->installEventFilter(focusWatcher);
        controller.setWindowFocused(window->isActive());
        workspaces.setWindowFocused(window->isActive());
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
    updateChecks.setInterval(workspace::updater::kPeriodicCheckIntervalMs);
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
    QObject::connect(&workspaces, &workspace::WorkspaceSessions::activationRequested,
                     &app, activate);
    QObject::connect(&preferences, &workspace::NativePreferences::localeChanged,
                     &desktop, &workspace::NativeDesktop::refreshLabels);
    QString badgeOwner;
    const auto updateUnread = [&controller, &desktop, &badgeOwner] {
        badgeOwner = controller.ownerKey();
        desktop.setUnreadCount(controller.desktopUnreadCount());
    };
    QObject::connect(&controller, &workspace::NativeController::streamsChanged, &desktop, updateUnread);
    QObject::connect(&controller, &workspace::NativeController::stateChanged, &desktop,
                     [&controller, &badgeOwner, updateUnread] {
        if (controller.ownerKey() != badgeOwner) updateUnread();
    });
    updateUnread();
    QObject::disconnect(earlyActivation);
    QObject::connect(&instance, &workspace::NativeInstance::activationRequested, &app,
        [&controller, activate](const QString &deepLink) {
            activate();
            if (!deepLink.isEmpty()) controller.openMessageLink(deepLink);
        });
    if (instance.activationPending()) activate();
    if (!startupDeepLink.isEmpty()) controller.openMessageLink(startupDeepLink);
    QTimer::singleShot(0, &workspaces, &workspace::WorkspaceSessions::start);
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
