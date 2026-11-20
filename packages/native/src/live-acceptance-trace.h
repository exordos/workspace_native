#pragma once

class QQuickWindow;
namespace workspace {
class ApiClient;
class NativeController;
class NativeCacheService;
class RealtimeClient;

// Linux acceptance works only with an anonymous pipe inherited from the runner.
void configureLiveAcceptanceNetwork();
void observeLiveAcceptance(QQuickWindow *window, ApiClient &api, NativeController &controller,
                           NativeCacheService &cache, RealtimeClient &realtime);
}
