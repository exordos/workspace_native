#include "native-app.h"
#include "native-controller.h"
#include "native-markdown.h"
#include "native-preferences.h"
#include "native-cache-service.h"
#include "avatar-images.h"
#include <QQuickImageProvider>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QResource>
#include <QTranslator>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <memory>

static void initializeNativeResources()
{
    Q_INIT_RESOURCE(native_icons);
    Q_INIT_RESOURCE(native_brand);
}

static void initializeLocaleResources()
{
    Q_INIT_RESOURCE(native_translations);
}

namespace workspace {

void bindNativeChatKeys(NativeChatKeyVault &keys, Session &session, ApiClient &api,
                        NativeController &controller, const QString &deviceName)
{
    struct Opening { QString owner; quint64 attempt = 0; bool pending = false; };
    const auto opening = std::make_shared<Opening>();
    const QPointer<NativeChatKeyVault> store(&keys);
    const QPointer<Session> identity(&session);
    const QPointer<ApiClient> transport(&api);
    const QPointer<NativeController> view(&controller);
    const auto synchronize = [store, identity, transport, view, deviceName, opening](bool retry) {
        if (!store || !view) return;
        const auto owner = identity && transport && identity->ready()
            ? SessionVault::accountId(transport->server(), identity->project(), identity->user()) : QString{};
        if (owner.isEmpty()) {
            if (opening->owner.isEmpty() && !opening->pending && !store->available()) return;
            const auto attempt = ++opening->attempt;
            opening->owner.clear(); opening->pending = false;
            view->setChatKeyStoreOpening(false);
            if (store && opening->attempt == attempt) store->close();
            return;
        }
        if (opening->owner == owner && (!retry || opening->pending
            || (store->available() && store->lease().owner == owner))) return;
        const auto attempt = ++opening->attempt;
        opening->owner = owner; opening->pending = true;
        view->setChatKeyStoreOpening(true);
        if (!store || !identity || !transport || !view || opening->attempt != attempt) return;
        store->open(transport->server(), identity->project(), identity->user(), deviceName,
            [store, view, opening, owner, attempt](NativeChatKeyVault::Failure) {
            if (!store || !view || opening->attempt != attempt || opening->owner != owner) return;
            opening->pending = false;
            view->setChatKeyStoreOpening(false);
        });
    };
    controller.useChatKeys(&keys);
    if (!store || !identity || !transport || !view) return;
    QObject::connect(&session, &Session::authenticated, &keys, [synchronize] { synchronize(false); });
    QObject::connect(&session, &Session::signedOut, &keys, [synchronize] { synchronize(false); });
    QObject::connect(&session, &Session::busyChanged, &keys, [synchronize](bool) { synchronize(false); });
    QObject::connect(&controller, &NativeController::chatKeyStoreOpenRequested, &keys,
                     [synchronize] { synchronize(true); });
    synchronize(false);
}

void bindNativeCachePreferences(NativeCacheService &cache, NativePreferences &preferences)
{
    const auto report = [&cache, &preferences] {
        preferences.reportCacheError(cache.error());
        preferences.reportCacheStatus(cache.maintenanceBusy(), cache.available(),
                                     cache.appliedDiskLimitBytes(), cache.diskUsageBytes());
    };
    QObject::connect(&cache, &NativeCacheService::errorChanged, &preferences, report);
    QObject::connect(&cache, &NativeCacheService::stateChanged, &preferences, report);
    const auto apply = [&cache, &preferences] {
        cache.setDiskLimitBytes(qint64(preferences.cacheLimitGb()) * 1000000000LL);
        preferences.reportCacheError(cache.error());
    };
    apply();
    report();
    QObject::connect(&preferences, &NativePreferences::cacheLimitGbChanged, &cache, apply);
}

void bindNativeLocale(QQmlEngine &engine, NativePreferences *preferences)
{
    initializeLocaleResources();
    auto *translator = new QTranslator(&engine);
    auto *qtTranslator = new QTranslator(&engine);
    const auto apply = [&engine, preferences, translator, qtTranslator] {
        QCoreApplication::removeTranslator(translator);
        QCoreApplication::removeTranslator(qtTranslator);
        const bool russian = preferences->locale() == "ru";
        QLocale::setDefault(QLocale(russian ? "ru_RU" : "en_US"));
        if (russian) {
            const auto translations = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
            // Qt deployment merges module catalogs into qt_ru.qm; a developer
            // SDK can instead provide separate qtbase_ru.qm and dependencies.
            if (qtTranslator->load("qt_ru", translations) || qtTranslator->load("qtbase_ru", translations))
                QCoreApplication::installTranslator(qtTranslator);
            if (translator->load(":/workspace/i18n/workspace_ru.qm"))
                QCoreApplication::installTranslator(translator);
        }
        engine.retranslate();
    };
    QObject::connect(preferences, &NativePreferences::localeChanged, &engine, apply);
    QObject::connect(&engine, &QObject::destroyed, translator, [translator, qtTranslator] {
        QCoreApplication::removeTranslator(translator);
        QCoreApplication::removeTranslator(qtTranslator);
    });
    apply();
}

void loadNativeUi(QQmlApplicationEngine &engine, NativeController *controller, NativePreferences *preferences, AvatarImages *avatars)
{
    initializeNativeResources();
    if (!preferences) preferences = new NativePreferences({}, &engine);
    const QPointer<NativeController> controllerGuard(controller);
    const QPointer<NativePreferences> preferencesGuard(preferences);
    controller->usePreferences(preferences);
    if (!controllerGuard || !preferencesGuard) return;
    preferences->setOwner(controller->ownerKey());
    bindNativeLocale(engine, preferences);
    QObject::connect(controller, &NativeController::stateChanged, preferences, [controller, preferences] {
        preferences->setOwner(controller->ownerKey());
    });
    QObject::connect(controller, &NativeController::reactionAdded, preferences,
                     [preferences](const QString &, const QString &emoji, const QString &owner,
                                   const QString &, const QString &) {
        preferences->recordQuickReactionUse(emoji, owner);
    });
    engine.rootContext()->setContextProperty("nativeController", controller);
    engine.rootContext()->setContextProperty("nativeMarkdown", new NativeMarkdown(&engine));
    engine.rootContext()->setContextProperty("nativePreferences", preferences);
    if (avatars) engine.addImageProvider("avatars", avatars->createImageProvider());
    engine.rootContext()->setContextProperty("nativeAvatars", avatars);
    engine.load(QUrl("qrc:/workspace/qml/Main.qml"));
}

}
