#include "api-client.h"
#include "avatar-images.h"
#include "file-client.h"
#include "native-app.h"
#include "native-controller.h"
#include "native-desktop.h"
#include "native-cache-service.h"
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
    // A direct SSL dependency keeps Qt's TLS backend and message crypto on the
    // bundled OpenSSL pair before transitive system libraries can load another.
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
        qCritical("The encryption runtime could not be initialized.");
        return 1;
    }
    app.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle("Basic");
    workspace::NativeInstance instance;
    const auto outcome = instance.acquireOrActivate();
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
    workspace::NativePreferences preferences;
    workspace::NativeCacheService cache;
    workspace::bindNativeCachePreferences(cache, preferences);
    workspace::NativeController controller(&api, &session);
#if defined(Q_OS_MACOS)
    const auto deviceName = QStringLiteral("Mac — Desktop");
#elif defined(Q_OS_WIN)
    const auto deviceName = QStringLiteral("Windows — Desktop");
#else
    const auto deviceName = QStringLiteral("Linux — Desktop");
#endif
    workspace::bindNativeChatKeys(chatKeys, session, api, controller, deviceName);
    controller.useCache(&cache);
    controller.useFiles(&files);
    workspace::RealtimeClient realtime(&api, &session);
    controller.useRealtime(&realtime);
    QQmlApplicationEngine engine;
    workspace::NativeUpdater updater;
    workspace::loadNativeUi(engine, &controller, &preferences, &avatars, &updater);
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QWindow *>(engine.rootObjects().first());
    workspace::NativeDesktop desktop(window);
    QString restartExecutable;
    const auto beginShutdown = [&cache, &chatKeys, &controller, &app, window] {
        window->hide();
        controller.setChatKeyStoreOpening(false);
        chatKeys.close();
        if (cache.stopped()) app.quit();
        else cache.shutdown();
    };
    QObject::connect(&app, &QGuiApplication::lastWindowClosed, &cache, beginShutdown);
    QObject::connect(&desktop, &workspace::NativeDesktop::quitRequested, &cache, beginShutdown);
    QObject::connect(&updater, &workspace::NativeUpdater::restartRequested, &cache,
                     [&restartExecutable, beginShutdown](const QString &executable) {
        restartExecutable = executable;
        beginShutdown();
    });
    QObject::connect(&desktop, &workspace::NativeDesktop::checkForUpdatesRequested, &updater,
                     [&updater, window] {
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
    QObject::connect(&cache, &workspace::NativeCacheService::shutdownFinished, &app, &QCoreApplication::quit);
    preferences.attachWindow(window);
    const auto activate = [&preferences] { preferences.activateWindow(); };
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
    QObject::connect(&instance, &workspace::NativeInstance::activationRequested, &app, activate);
    if (instance.activationPending()) activate();
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
