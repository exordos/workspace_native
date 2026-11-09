#pragma once

#include <QString>
#include <functional>

class QQmlApplicationEngine;
class QQmlEngine;
class QObject;

namespace workspace {
class NativeController;
class NativePreferences;
class AvatarImages;
class MessageImages;
class NativeCacheService;
class NativeChatKeyVault;
class ApiClient;
class Session;
class NativeUpdater;
class NativeDrafts;
class NativeDraftService;
// Flush composer receipts first. A failed flush leaves both workers available
// for retry; success is reported only after both workers have stopped.
void shutdownNativeStorage(NativeDrafts &drafts, NativeDraftService &draftStorage,
                           NativeCacheService &cache, QObject *receiver,
                           std::function<void(bool)> done);
void bindNativeLocale(QQmlEngine &engine, NativePreferences *preferences);
void bindNativeCachePreferences(NativeCacheService &cache, NativePreferences &preferences);
void bindNativeChatKeys(NativeChatKeyVault &keys, Session &session, ApiClient &api,
                        NativeController &controller, const QString &deviceName);
void loadNativeUi(QQmlApplicationEngine &engine, NativeController *controller,
                  NativePreferences *preferences = nullptr, AvatarImages *avatars = nullptr,
                  NativeUpdater *updater = nullptr, MessageImages *messageImages = nullptr);
}
