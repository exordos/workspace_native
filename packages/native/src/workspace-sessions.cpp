#include "workspace-sessions.h"

#include "api-client.h"
#include "native-controller.h"
#include "native-preferences.h"
#include "realtime-client.h"
#include "session.h"

#include <QSet>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <utility>

namespace workspace {

struct WorkspaceSessions::Runtime final : QObject {
    Runtime(const SavedAccount &account, QObject *parent)
        : QObject(parent), id(account.id), session(&api), controller(&api, &session),
          realtime(&api, &session)
    {
        controller.notifications()->setWorkspaceLabel(account.server.host());
    }

    const QString id;
    ApiClient api;
    Session session;
    NativeController controller;
    RealtimeClient realtime;
    bool started = false;
};

WorkspaceSessions::WorkspaceSessions(Session &windowSession, NativeController &window,
                                     SessionVault &vault, RealtimeCursorStore &cursors)
    : m_windowSession(windowSession), m_window(window), m_vault(vault), m_cursors(cursors)
{
    m_windowSession.m_resumeWorkspace = [this](const QString &id) { select(id); };
    m_window.setRealtimeMode(true);
    connect(&m_vault, &SessionVault::accountsChanged, this, [this] {
        QTimer::singleShot(0, this, &WorkspaceSessions::reconcile);
    });
    connect(&m_windowSession, &Session::signedOut, this, [this] {
        m_selected.clear();
        m_destination = {};
        m_window.m_realtime = nullptr;
        m_window.m_workspaceNotifications = nullptr;
        QTimer::singleShot(0, this, &WorkspaceSessions::reconcile);
    });
    connect(&m_windowSession, &Session::authenticated, this, [this] {
        // A login wizard's first verified grant belongs to this process already.
        // Hand over renewal before opening another grant for that account.
        if (m_windowSession.m_renewalSource) return;
        const auto id = m_windowSession.currentSavedAccountId();
        if (id.isEmpty()) return;
        m_selected = id;
        takeOver(*ensure({id, m_window.m_api->server(), m_windowSession.project(),
                          m_windowSession.user(), m_windowSession.loginName(), {}}));
    });
    connect(&m_window, &NativeController::resynchronizeRequested, this, [this] {
        // Refresh the window's catalog without resetting the delivery cursor.
        m_window.reloadSnapshot();
    });
    connect(&m_window, &NativeController::snapshotReloaded, this, [this] {
        const auto *runtime = m_runtimes.value(m_windowSession.currentSavedAccountId());
        if (runtime && isPresented(*runtime))
            m_window.publishRealtimeState(runtime->controller.m_syncState);
        openDestination();
    });
    connect(&m_window, &NativeController::snapshotFailed, this, &WorkspaceSessions::openDestination);
}

WorkspaceSessions::~WorkspaceSessions()
{
    m_windowSession.m_resumeWorkspace = {};
    for (auto *runtime : std::as_const(m_runtimes))
        runtime->controller.notifications()->setTransport(nullptr);
}

void WorkspaceSessions::start()
{
    if (auto *preferences = m_window.m_preferences.data()) {
        connect(preferences, &NativePreferences::sidebarSortModeChanged, this, [this] {
            for (auto *runtime : std::as_const(m_runtimes))
                refreshSidebarOrder(*runtime, isPresented(*runtime));
        });
        connect(preferences, &NativePreferences::localeChanged, this, [this] {
            if (auto *runtime = m_runtimes.value(m_windowSession.currentSavedAccountId());
                runtime && isPresented(*runtime))
                refreshSidebarOrder(*runtime, true);
        });
    }
    m_windowSession.resumeCurrentAccount();
    reconcile();
}

void WorkspaceSessions::setNotificationTransport(NotificationTransport *transport)
{
    m_transport = transport;
    for (auto *runtime : std::as_const(m_runtimes))
        runtime->controller.notifications()->setTransport(transport);
}

void WorkspaceSessions::setPowerWatcher(PowerWatcher *watcher)
{
    m_powerWatcher = watcher;
    for (auto *runtime : std::as_const(m_runtimes))
        runtime->controller.setPowerWatcher(watcher);
}

void WorkspaceSessions::setWindowFocused(bool focused)
{
    m_windowFocused = focused;
    for (auto *runtime : std::as_const(m_runtimes))
        runtime->controller.notifications()->setWindowFocused(focused);
}

WorkspaceSessions::Runtime *WorkspaceSessions::ensure(const QString &id)
{
    if (auto *existing = m_runtimes.value(id)) return existing;
    const auto accounts = m_vault.accounts();
    const auto entry = std::find_if(accounts.cbegin(), accounts.cend(),
        [&](const SavedAccount &account) { return account.id == id; });
    return entry == accounts.cend() ? nullptr : ensure(*entry);
}

WorkspaceSessions::Runtime *WorkspaceSessions::ensure(const SavedAccount &account)
{
    if (auto *existing = m_runtimes.value(account.id)) return existing;
    auto *runtime = new Runtime(account, this);
    m_runtimes.insert(account.id, runtime);
    runtime->session.setVault(&m_vault);
    runtime->session.m_selectRememberedAccount = false;
    runtime->controller.m_backgroundWorkspace = true;
    refreshSidebarOrder(*runtime, true);
    runtime->controller.sidebarStreamListModel();
    runtime->controller.m_notificationViewport = &m_window;
    runtime->controller.setWindowVisible(false);
    runtime->controller.setWindowFocused(false);
    runtime->controller.setPowerWatcher(m_powerWatcher);
    runtime->realtime.useCursorStore(&m_cursors);
    runtime->controller.useRealtime(&runtime->realtime);
    auto *notifications = runtime->controller.notifications();
    notifications->setTransport(m_transport);
    notifications->setWindowFocused(m_windowFocused);
    connect(&runtime->session, &Session::authenticated, this, [this, runtime] {
        authenticated(*runtime);
    });
    connect(&runtime->session, &Session::startupIdentityResolved, this, [this, runtime] {
        if (m_selected != runtime->id || m_windowSession.ready()) return;
        const auto selection = m_selected;
        const auto destination = m_destination;
        m_windowSession.presentLocalIdentity(runtime->session);
        m_selected = selection;
        m_destination = destination;
    });
    connect(&runtime->session, &Session::renewalUnavailableChanged, this, [this, runtime] {
        if (m_windowSession.m_renewalSource != &runtime->session) return;
        m_windowSession.m_renewalUnavailable = runtime->session.renewalUnavailable();
        emit m_windowSession.renewalUnavailableChanged();
        if (runtime->session.renewalUnavailable()) emit m_windowSession.busyChanged(false);
    });
    connect(&runtime->session, &Session::signedOut, this, [this, runtime] {
        m_window.publishSessionProjection(runtime->id, {});
    });
    connect(&runtime->session, &Session::error, this, [this, runtime](const QString &message) {
        if (!runtime->session.ready()) runtime->started = false;
        if (m_selected != runtime->id && m_windowSession.m_renewalSource != &runtime->session) return;
        if (!runtime->session.ready()) {
            if (m_windowSession.m_renewalSource == &runtime->session)
                m_windowSession.detach();
            m_selected.clear();
            m_destination = {};
        }
        emit m_windowSession.error(message);
    });
    connect(&runtime->realtime, &RealtimeClient::connectionChanged, this, [this, runtime](const QString &state) {
        if (isPresented(*runtime)) m_window.publishRealtimeState(state);
    });
    connect(&runtime->realtime, &RealtimeClient::eventReceived, this, [this, runtime](const QJsonObject &event) {
        if (!isPresented(*runtime)) return;
        if (!m_window.applyRealtimeEvent(event)) m_window.reloadSnapshot();
        if (isPresented(*runtime)) emit eventReceived(event);
    });
    connect(&runtime->controller, &NativeController::streamsChanged, this, [this, runtime] {
        updateUnread(*runtime);
        if (isPresented(*runtime)) m_window.publishWorkspaceCatalog(runtime->controller);
    });
    connect(notifications, &NotificationService::activationRequested, this, &WorkspaceSessions::activationRequested);
    connect(notifications, &NotificationService::soundRequested, this, &WorkspaceSessions::soundRequested);
    connect(notifications, &NotificationService::messageRouteRequested, this,
        [this, runtime](const QString &message) {
            if (!runtime->session.ready() || !isUuid(message)) return;
            m_destination = {runtime->id, message};
            if (isPresented(*runtime)) {
                openDestination();
            } else {
                m_selected = runtime->id;
                present(*runtime);
            }
        });
    return runtime;
}

void WorkspaceSessions::reconcile()
{
    QSet<QString> remembered;
    for (const auto &account : m_vault.accounts()) {
        remembered.insert(account.id);
        auto *runtime = ensure(account.id);
        if (runtime->started) continue;
        if (m_windowSession.ready() && !m_windowSession.m_renewalSource
            && m_windowSession.currentSavedAccountId() == account.id) {
            takeOver(*runtime);
            continue;
        }
        runtime->started = true;
        runtime->session.resumeSavedAccount(account.id);
    }
    for (const auto &id : m_runtimes.keys()) {
        if (remembered.contains(id) || isPresented(*m_runtimes.value(id))) continue;
        auto *runtime = m_runtimes.take(id);
        if (m_windowSession.m_renewalSource == &runtime->session) m_windowSession.detach();
        runtime->session.detach();
        runtime->deleteLater();
    }
}

void WorkspaceSessions::select(const QString &id)
{
    auto *runtime = ensure(id);
    if (!runtime) return;
    m_destination = {};
    m_selected = id;
    if (isPresented(*runtime)) return;
    if (runtime->session.ready()) {
        present(*runtime);
        return;
    }
    if (runtime->session.m_savedAccountId == id && !runtime->session.project().isEmpty()
        && !m_windowSession.ready() && m_windowSession.m_renewalSource != &runtime->session) {
        m_windowSession.presentLocalIdentity(runtime->session);
        m_selected = id;
    }
    if (!runtime->started) {
        runtime->started = true;
        runtime->session.resumeSavedAccount(id);
    } else if (runtime->session.renewalUnavailable()) {
        runtime->session.requestRefresh();
    }
}

bool WorkspaceSessions::isPresented(const Runtime &runtime) const
{
    return m_windowSession.ready() && m_windowSession.m_renewalSource == &runtime.session;
}

void WorkspaceSessions::authenticated(Runtime &runtime)
{
    if (m_selected == runtime.id || isPresented(runtime)) present(runtime);
    QTimer::singleShot(0, this, &WorkspaceSessions::reconcile);
}

void WorkspaceSessions::takeOver(Runtime &runtime)
{
    runtime.started = true;
    m_window.m_realtime = &runtime.realtime;
    m_window.m_workspaceNotifications = runtime.controller.notifications();
    runtime.session.assumeRenewalFrom(m_windowSession);
}

void WorkspaceSessions::present(Runtime &runtime)
{
    const bool switching = !isPresented(runtime);
    if (switching) {
        if (auto *previous = m_runtimes.value(m_windowSession.currentSavedAccountId());
            previous && isPresented(*previous))
            rememberSidebar(*previous);
    }
    const auto selected = m_selected;
    const auto destination = m_destination;
    const bool keepShell = switching && m_windowSession.ready();
    m_window.m_workspaceChanging = keepShell;
    m_windowSession.present(runtime.session);
    m_window.m_workspaceChanging = false;
    m_selected = selected;
    m_destination = destination;
    m_window.m_realtime = &runtime.realtime;
    m_window.m_workspaceNotifications = runtime.controller.notifications();
    if (switching) m_window.m_selectedFolder = runtime.controller.m_selectedFolder;
    m_window.publishWorkspaceCatalog(runtime.controller);
    if (!runtime.controller.m_syncState.isEmpty())
        m_window.publishRealtimeState(runtime.controller.m_syncState);
    else if (keepShell)
        emit m_window.stateChanged();
    if (switching) {
        const auto id = runtime.id;
        QTimer::singleShot(0, this, [this, id] {
            if (m_windowSession.ready() && m_windowSession.currentSavedAccountId() == id)
                m_vault.setCurrentAccountId(id);
        });
    }
    openDestination();
}

void WorkspaceSessions::rememberSidebar(Runtime &runtime)
{
    auto &sidebar = runtime.controller;
    // The projection belongs to the session being hidden. Retain its selected
    // folder and already loaded topic branches under the same account.
    sidebar.m_selectedFolder = m_window.m_selectedFolder;
    for (auto it = m_window.m_inboxTopics.cbegin(); it != m_window.m_inboxTopics.cend(); ++it) {
        sidebar.m_inboxTopics.insert(it.key(), it.value());
        if (m_window.m_inboxStaleTopics.contains(it.key())) sidebar.m_inboxStaleTopics.insert(it.key());
        else sidebar.m_inboxStaleTopics.remove(it.key());
    }
    if (m_window.m_topicsVerified && !m_window.m_stream.isEmpty()) {
        sidebar.m_inboxTopics.insert(m_window.m_stream, m_window.m_topics);
        sidebar.m_inboxStaleTopics.remove(m_window.m_stream);
    }
    emit sidebar.sidebarStreamsChanged();
}

void WorkspaceSessions::refreshSidebarOrder(Runtime &runtime, bool updateLocale)
{
    auto &sidebar = runtime.controller;
    const auto mode = m_window.sidebarSortMode();
    const auto locale = updateLocale ? m_window.sidebarSortLocale() : sidebar.m_backgroundSidebarLocale;
    if (mode == sidebar.m_backgroundSidebarSortMode && locale == sidebar.m_backgroundSidebarLocale) return;
    sidebar.m_backgroundSidebarSortMode = mode;
    sidebar.m_backgroundSidebarLocale = locale;
    emit sidebar.sidebarStreamsChanged();
}

void WorkspaceSessions::updateUnread(const Runtime &runtime)
{
    if (!runtime.session.ready()) return;
    qint64 count = 0;
    for (const auto &stream : runtime.controller.m_streams)
        if (!stream.archived) count += qMax(0, stream.activeUnread);
    const QVariantMap totals{{QStringLiteral("00000000-0000-0000-0000-000000000000"),
        int(qMin<qint64>(count, std::numeric_limits<int>::max()))}};
    m_window.publishSessionProjection(runtime.id, {{QStringLiteral("unreadByFolderId"), totals}});
}

void WorkspaceSessions::openDestination()
{
    if (m_window.m_snapshotActive || !m_windowSession.ready() || m_destination.message.isEmpty()
        || m_windowSession.currentSavedAccountId() != m_destination.account) return;
    const auto target = std::exchange(m_destination, Destination{});
    m_window.cancelConversationRestore();
    ++m_window.m_deepLinkSerial;
    m_window.beginAnchorLinkJump(target.message);
}

void WorkspaceSessions::clickNotification(const QString &tag)
{
    for (auto *runtime : std::as_const(m_runtimes))
        if (runtime->controller.notifications()->click(tag)) return;
}

} // namespace workspace
