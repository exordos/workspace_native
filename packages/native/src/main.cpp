#include "api-client.h"
#include "avatar-images.h"
#include "message-images.h"
#include "file-client.h"
#include "native-app.h"
#include "native-controller.h"
#include "native-desktop.h"
#include "native-cache-service.h"
#include "native-drafts.h"
#include "native-deeplink.h"
#include "native-instance.h"
#include "native-updater.h"
#include "native-preferences.h"
#include "realtime-client.h"
#include "session.h"
#include "session-vault.h"

#include <QApplication>
#include <QIcon>
#include <QResource>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QProcess>
#include <QTimer>
#include <QWindow>
#include <openssl/ssl.h>
#include <limits>

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
    workspace::NativeInstance instance;
    const auto outcome = instance.acquireOrActivate(startupDeepLink);
    if (outcome == workspace::NativeInstance::Outcome::Secondary) return 0;
    if (outcome == workspace::NativeInstance::Outcome::Failed) {
        qCritical("%s", qPrintable(instance.error()));
        return 1;
    }
    Q_INIT_RESOURCE(native_app_icon);
    QGuiApplication::setWindowIcon(QIcon(":/workspace/brand/icon.png"));
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
    workspace::RealtimeClient realtime(&api, &session);
    controller.useRealtime(&realtime);
    QQmlApplicationEngine engine;
    workspace::NativeUpdater updater;
    workspace::loadNativeUi(engine, &controller, &preferences, &avatars, &updater, &messageImages);
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QWindow *>(engine.rootObjects().first());
    workspace::NativeDesktop desktop(window);
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
    QObject::connect(&preferences, &workspace::NativePreferences::localeChanged,
                     &desktop, &workspace::NativeDesktop::refreshLabels);
    QString badgeOwner;
    const auto updateUnread = [&controller, &desktop, &badgeOwner] {
        badgeOwner = controller.ownerKey();
        qint64 count = 0;
        if (controller.authenticated()) {
            for (const auto &value : controller.streams()) {
                const auto unread = qMax<qint64>(0, value.toMap().value("unread").toLongLong());
                count += qMin(unread, std::numeric_limits<qint64>::max() - count);
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
