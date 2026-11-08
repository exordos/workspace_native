#pragma once

#include <QString>

class QQmlApplicationEngine;
class QQmlEngine;

namespace workspace {
class NativeController;
class NativePreferences;
class AvatarImages;
class NativeCacheService;
class NativeChatKeyVault;
class ApiClient;
class Session;
void bindNativeLocale(QQmlEngine &engine, NativePreferences *preferences);
void bindNativeCachePreferences(NativeCacheService &cache, NativePreferences &preferences);
void bindNativeChatKeys(NativeChatKeyVault &keys, Session &session, ApiClient &api,
                        NativeController &controller, const QString &deviceName);
void loadNativeUi(QQmlApplicationEngine &engine, NativeController *controller,
                  NativePreferences *preferences = nullptr, AvatarImages *avatars = nullptr);
}
