#include "native-notifications.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QRegularExpression>

#if defined(Q_OS_LINUX) && defined(WORKSPACE_NATIVE_HAVE_XCB)
#include <xcb/xcb.h>
#define WORKSPACE_NATIVE_ATTENTION_XCB 1
#endif

namespace workspace {

namespace {

const char *kTriggerMetadataUnavailable = "metadata_unavailable";
const char *kTriggerDm = "dm";
const char *kTriggerWildcard = "wildcard_mention";
const char *kTriggerMention = "mention";
const char *kTriggerFollowedTopic = "followed_topic";
const char *kTriggerStream = "stream";

// The reference shell truncates both segments to MAX_NOTIFICATION_LENGTH = 200
// in notifications:show (main.ts:1118-1127).
constexpr qsizetype kMaxNotificationLength = 200;
constexpr int kProcessedCap = 2048;      // owner::message decisions remembered
constexpr int kPendingCap = 64;          // candidates waiting on metadata
constexpr int kAggregateTagsCap = 128;   // live aggregate tags
constexpr int kAggregateRowsPerTag = 64; // retained rows per aggregate bucket

QString clip200(const QString &text)
{
    return text.left(static_cast<qsizetype>(kMaxNotificationLength));
}

} // namespace

QString notificationSeparator()
{
    return QString::fromUtf8(" \xC2\xB7 ");
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

namespace notification_policy {

QString wildcardMentionCheck(const QString &content)
{
    // Ported verbatim from workspace-desktop-notifications.lib.ts:
    // /(^|[\s([{"'.,!?;:])@(all|everyone|channel)\b/iu
    static const QRegularExpression pattern(
        QStringLiteral("(^|[\\s([{\"'.,!?;:])@(all|everyone|channel)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(content).hasMatch() ? QString::fromLatin1(kTriggerWildcard) : QString();
}

QString classifyTrigger(const NotificationCandidate &candidate)
{
    if (candidate.streamPrivate)
        return QString::fromLatin1(kTriggerDm);
    if (!wildcardMentionCheck(candidate.content).isEmpty())
        return QString::fromLatin1(kTriggerWildcard);
    if (candidate.mentioned)
        return QString::fromLatin1(kTriggerMention);
    if (candidate.topicMode == QLatin1String("follow"))
        return QString::fromLatin1(kTriggerFollowedTopic);
    return QString::fromLatin1(kTriggerStream);
}

namespace {

bool isMuted(const NotificationCandidate &candidate)
{
    // isWorkspaceDesktopNotificationMuted: topic mute wins, topic unmute or
    // follow un-mutes, otherwise the stream mode decides.
    if (candidate.topicMode == QLatin1String("mute"))
        return true;
    if (candidate.topicMode == QLatin1String("unmute") || candidate.topicMode == QLatin1String("follow"))
        return false;
    return candidate.streamMode == QLatin1String("muted");
}

bool enabledForTrigger(const QString &trigger, const NotificationCandidate &candidate)
{
    if (trigger == QLatin1String(kTriggerMetadataUnavailable)
        || trigger == QLatin1String(kTriggerDm)
        || trigger == QLatin1String(kTriggerMention)
        || trigger == QLatin1String(kTriggerWildcard))
        return true;
    if (trigger == QLatin1String(kTriggerFollowedTopic))
        return candidate.topicMode == QLatin1String("follow");
    if (trigger == QLatin1String(kTriggerStream))
        return candidate.streamMode == QLatin1String("all_messages");
    return false;
}

} // namespace

NotificationDecision shouldNotify(const NotificationCandidate &candidate, bool metadataUnavailable)
{
    const auto trigger = metadataUnavailable
        ? QString::fromLatin1(kTriggerMetadataUnavailable)
        : classifyTrigger(candidate);
    const auto viewport = candidate.viewport;
    const bool blocked = candidate.isOwn || candidate.read || isMuted(candidate)
        || (viewport.windowFocused && viewport.conversationOnScreen)
        || !enabledForTrigger(trigger, candidate);
    return {!blocked, trigger};
}

} // namespace notification_policy

// ---------------------------------------------------------------------------
// Aggregate registry
// ---------------------------------------------------------------------------

QString NotificationAggregateRegistry::aggregateTag(const QString &owner, const QString &bucketKey)
{
    return QStringLiteral("bucket:") + owner + QStringLiteral("::") + bucketKey;
}

QString NotificationAggregateRegistry::fallbackTag(const QString &owner, const QString &messageUuid)
{
    return QStringLiteral("msg:") + owner + QStringLiteral("::message:") + messageUuid;
}

QString NotificationAggregateRegistry::bucketKeyFor(const NotificationCandidate &candidate)
{
    // DM bucket = owner + stream conversation; channel bucket = owner + topic
    // + author (notification-aggregate-registry.lib.ts).
    if (candidate.streamPrivate)
        return candidate.streamUuid;
    if (candidate.topicUuid.isEmpty() || candidate.authorUuid.isEmpty())
        return {};
    return candidate.topicUuid + QStringLiteral(":author:") + candidate.authorUuid;
}

std::optional<NotificationSnapshot> NotificationAggregateRegistry::upsert(
    const NotificationCandidate &candidate, const NotificationTitleContext &title)
{
    const auto bucket = bucketKeyFor(candidate);
    if (bucket.isEmpty())
        return {};
    const auto tag = aggregateTag(candidate.owner, bucket);
    const auto scope = candidate.owner + QStringLiteral("::message:") + candidate.messageUuid;
    const auto existing = m_messageToTag.constFind(scope);
    if (existing != m_messageToTag.cend() && existing.value() != tag)
        return {}; // A message may not move between buckets.
    auto &entry = m_byTag[tag];
    entry.messages.insert(candidate.messageUuid, Entry::Row{
        candidate.content.trimmed(), candidate.streamUuid, candidate.topicUuid, title,
        candidate.createdMicros});
    ++entry.total;
    m_messageToTag.insert(scope, tag);
    // A long-running aggregate must not retain every body it ever saw: keep
    // the newest rows, drop the coldest ones along with their index entries.
    while (entry.messages.size() > kAggregateRowsPerTag) {
        auto oldest = entry.messages.begin();
        for (auto it = entry.messages.begin(); it != entry.messages.end(); ++it)
            if (it.value().createdMicros < oldest.value().createdMicros)
                oldest = it;
        m_messageToTag.remove(
            candidate.owner + QStringLiteral("::message:") + oldest.key());
        entry.messages.erase(oldest);
    }
    if (m_byTag.size() > kAggregateTagsCap) {
        // Evict the entry whose newest row is coldest: aggregate banners are a
        // rolling window, not an archive (bounds tenant-driven client memory).
        QString coldestTag;
        qint64 coldest = 0;
        for (auto it = m_byTag.constBegin(); it != m_byTag.constEnd(); ++it) {
            qint64 newest = 0;
            for (const auto &row : it->messages)
                newest = qMax(newest, row.createdMicros);
            if (coldestTag.isEmpty() || newest < coldest) { coldestTag = it.key(); coldest = newest; }
        }
        if (!coldestTag.isEmpty() && coldestTag != tag) {
            consumeByTag(coldestTag);
            // The desktop banner of an evicted aggregate would otherwise stay
            // open untracked forever (no tag close, no future update).
            m_evicted.append(coldestTag);
        }
    }
    return snapshot(tag, entry);
}

std::optional<NotificationSnapshot> NotificationAggregateRegistry::peek(const QString &tag) const
{
    const auto entry = m_byTag.constFind(tag);
    return entry == m_byTag.cend() ? std::optional<NotificationSnapshot>{}
                                   : snapshot(tag, entry.value());
}

std::optional<NotificationSnapshot> NotificationAggregateRegistry::snapshot(
    const QString &tag, const Entry &entry) const
{
    const Entry::Row *latest = nullptr;
    QString latestUuid;
    for (auto it = entry.messages.constBegin(); it != entry.messages.constEnd(); ++it) {
        const auto &row = it.value();
        const bool newer = latest == nullptr || row.createdMicros > latest->createdMicros
            || (row.createdMicros == latest->createdMicros && it.key() > latestUuid);
        if (newer) { latest = &row; latestUuid = it.key(); }
    }
    if (latest == nullptr)
        return {};
    NotificationSnapshot snap;
    snap.tag = tag;
    snap.count = int(entry.total);
    snap.lastMessageUuid = latestUuid;
    snap.body = latest->body;
    snap.routeStream = latest->routeStream;
    snap.routeTopic = latest->routeTopic;
    snap.titleContext = latest->title;
    return snap;
}

QList<QString> NotificationAggregateRegistry::consumeByTag(const QString &tag)
{
    QList<QString> uuids;
    auto entry = m_byTag.find(tag);
    if (entry == m_byTag.end())
        return uuids;
    uuids = entry->messages.keys();
    m_byTag.erase(entry);
    for (auto it = m_messageToTag.begin(); it != m_messageToTag.end();) {
        if (it.value() == tag) it = m_messageToTag.erase(it);
        else ++it;
    }
    return uuids;
}

NotificationReadOutcome NotificationAggregateRegistry::consumeRead(
    const QList<QString> &messageUuids, const QString &owner)
{
    NotificationReadOutcome outcome;
    QHash<QString, QSet<QString>> readByTag;
    QSet<QString> seen;
    for (const auto &messageUuid : messageUuids) {
        if (messageUuid.isEmpty() || seen.contains(messageUuid))
            continue;
        seen.insert(messageUuid);
        const auto scope = owner + QStringLiteral("::message:") + messageUuid;
        const auto tag = m_messageToTag.value(scope);
        if (tag.isEmpty()) {
            outcome.untrackedMessageUuids.append(messageUuid);
            continue;
        }
        readByTag[tag].insert(messageUuid);
    }
    for (auto it = readByTag.constBegin(); it != readByTag.constEnd(); ++it) {
        const auto snap = peek(it.key());
        if (!snap || it.value().contains(snap->lastMessageUuid)) {
            consumeByTag(it.key());
            outcome.closedTags.append(it.key());
            continue;
        }
        auto entry = m_byTag.find(it.key());
        for (const auto &messageUuid : it.value()) {
            if (entry != m_byTag.end() && entry->messages.remove(messageUuid))
                entry->total = qMax<qint64>(0, entry->total - 1);
            m_messageToTag.remove(owner + QStringLiteral("::message:") + messageUuid);
        }
        if (entry != m_byTag.end() && entry->messages.isEmpty())
            m_byTag.erase(entry);
    }
    return outcome;
}

QStringList NotificationAggregateRegistry::dropOwner(const QString &owner)
{
    const auto prefix = QStringLiteral("bucket:") + owner + QStringLiteral("::");
    QStringList drained;
    for (auto it = m_byTag.begin(); it != m_byTag.end();) {
        if (it.key().startsWith(prefix)) { drained << it.key(); it = m_byTag.erase(it); }
        else ++it;
    }
    for (auto it = m_messageToTag.begin(); it != m_messageToTag.end();) {
        if (it.value().startsWith(prefix)) it = m_messageToTag.erase(it);
        else ++it;
    }
    return drained;
}

int NotificationAggregateRegistry::count(const QString &tag) const
{
    const auto entry = m_byTag.constFind(tag);
    return entry == m_byTag.cend() ? 0 : int(entry->total);
}

QStringList NotificationAggregateRegistry::tags() const
{
    return m_byTag.keys();
}

// ---------------------------------------------------------------------------
// Dedup
// ---------------------------------------------------------------------------

bool NotificationDedup::contains(const QString &owner, const QString &messageUuid) const
{
    return m_keys.contains(owner + QStringLiteral("::") + messageUuid);
}

void NotificationDedup::insert(const QString &owner, const QString &messageUuid)
{
    const auto key = owner + QStringLiteral("::") + messageUuid;
    if (m_keys.contains(key))
        return;
    m_order.append(key);
    m_keys.insert(key);
    while (m_order.size() > kMaxRecentKeys)
        m_keys.remove(m_order.takeFirst());
}

void NotificationDedup::clear()
{
    m_order.clear();
    m_keys.clear();
}

// ---------------------------------------------------------------------------
// Service
// ---------------------------------------------------------------------------

qint64 NotificationService::nowMs()
{
    static const QElapsedTimer clock = [] {
        QElapsedTimer timer;
        timer.start();
        return timer;
    }();
    return clock.elapsed();
}

NotificationService::NotificationService(QObject *parent) : QObject(parent)
{
    m_graceTimer.setSingleShot(true);
    connect(&m_graceTimer, &QTimer::timeout, this, &NotificationService::retryPending);
}

void NotificationService::setMetadataGrace(int graceMs, int retryMs)
{
    m_graceMs = graceMs;
    m_retryMs = retryMs;
}

void NotificationService::setWindowFocused(bool focused)
{
    m_windowFocused = focused;
    // The reference clears the flash on every focus event (main.ts:437).
    if (focused && m_transport)
        m_transport->clearAttention();
}

void NotificationService::ingest(const NotificationCandidate &candidate)
{
    if (!m_transport)
        return;
    const auto key = candidate.owner + QStringLiteral("::") + candidate.messageUuid;
    if (m_processed.contains(key) || m_pending.contains(key))
        return;
    auto resolved = candidate;
    const bool metadataKnown = m_resolver ? m_resolver(resolved) : true;
    if (!metadataKnown) {
        m_pending.insert(key, Pending{resolved, nowMs()});
        while (m_pending.size() > kPendingCap) {
            auto oldest = m_pending.begin();
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
                if (it->observedMs < oldest->observedMs) oldest = it;
            m_pending.erase(oldest);
        }
        scheduleGraceRetry();
        return;
    }
    ingestNow(resolved);
}

void NotificationService::ingestNow(const NotificationCandidate &candidate)
{
    const auto key = candidate.owner + QStringLiteral("::") + candidate.messageUuid;
    m_pending.remove(key);
    if (m_processed.contains(key))
        return;
    if (m_dedup.contains(candidate.owner, candidate.messageUuid)) {
        markProcessed(key);
        return;
    }
    const auto decision = notification_policy::shouldNotify(candidate, false);
    if (!decision.notify) {
        markProcessed(key);
        return;
    }
    auto named = candidate;
    if (named.senderName.trimmed().isEmpty())
        named.senderName = tr("New message");
    NotificationTitleContext title;
    if (named.streamPrivate) {
        title.direct = true;
        title.senderName = named.senderName;
        title.conversationName = named.streamName.trimmed().isEmpty()
            ? named.senderName : named.streamName.trimmed();
    } else {
        title.senderName = named.senderName;
        title.channelName = named.streamName.trimmed().isEmpty()
            ? named.senderName : named.streamName.trimmed();
        title.topicName = named.topicName.trimmed();
    }
    const auto snap = m_registry.upsert(named, title);
    // Aggregate eviction must not leave the evicted desktop banner open and
    // untracked: close it on the transport as the registry drops it.
    for (const auto &evicted : m_registry.takeEvicted()) {
        if (snap && evicted == snap->tag)
            continue;
        m_transport->closeByTag(evicted);
    }
    if (!snap) {
        markProcessed(key);
        return;
    }
    m_transport->show(snap->tag, formatTitle(snap->titleContext, snap->count), snap->body);
    if (!m_windowFocused)
        m_transport->requestAttention();
    markProcessed(key);
    m_dedup.insert(candidate.owner, candidate.messageUuid);
}

void NotificationService::showGeneric(const NotificationCandidate &candidate)
{
    if (!m_transport)
        return;
    const auto tag = NotificationAggregateRegistry::fallbackTag(candidate.owner, candidate.messageUuid);
    const auto body = m_genericBody ? m_genericBody() : QString();
    m_transport->show(tag, tr("You have a new message"), body);
    m_fallbackRoutes.insert(tag, qMakePair(candidate.streamUuid, candidate.topicUuid));
    if (m_liveFallbackTags[candidate.owner].size() < 256)
        m_liveFallbackTags[candidate.owner].insert(tag);
    if (!m_windowFocused)
        m_transport->requestAttention();
    markProcessed(candidate.owner + QStringLiteral("::") + candidate.messageUuid);
    m_dedup.insert(candidate.owner, candidate.messageUuid);
}

void NotificationService::scheduleGraceRetry()
{
    if (m_pending.isEmpty())
        return;
    qint64 earliest = -1;
    for (const auto &pending : m_pending) {
        const auto remaining = m_graceMs - (nowMs() - pending.observedMs);
        earliest = earliest < 0 ? remaining : qMin(earliest, remaining);
    }
    m_graceTimer.start(qMax<long long>(qMin<long long>(m_retryMs, qMax<long long>(earliest, 1)), 1));
}

void NotificationService::retryPending()
{
    const auto snapshot = m_pending; // ingestNow/showGeneric mutate m_pending
    qint64 earliest = -1;
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        const auto &key = it.key();
        if (!m_pending.contains(key) || m_processed.contains(key)) {
            m_pending.remove(key);
            continue;
        }
        auto candidate = it->candidate;
        const bool metadataKnown = m_resolver ? m_resolver(candidate) : true;
        if (metadataKnown) {
            ingestNow(candidate);
            continue;
        }
        const auto elapsed = nowMs() - it->observedMs;
        if (elapsed >= m_graceMs) {
            m_pending.remove(key);
            showGeneric(candidate);
        } else {
            const auto remaining = m_graceMs - elapsed;
            earliest = earliest < 0 ? remaining : qMin(earliest, remaining);
        }
    }
    if (earliest >= 0 && !m_pending.isEmpty())
        m_graceTimer.start(qMax<long long>(qMin<long long>(m_retryMs, earliest), 1));
}

void NotificationService::markProcessed(const QString &key)
{
    if (m_processed.contains(key))
        return;
    m_processed.insert(key);
    m_processedOrder.append(key);
    while (m_processedOrder.size() > kProcessedCap)
        m_processed.remove(m_processedOrder.takeFirst());
}

void NotificationService::onOwnerRetired(const QString &owner)
{
    if (owner.isEmpty())
        return;
    // The reference signs out by closing every live banner of the owner first
    // (closeAllActiveMessageNotifications), so old message content can never
    // outlive the account that produced it.
    if (m_transport) {
        for (const auto &tag : m_registry.dropOwner(owner))
            m_transport->closeByTag(tag);
    } else {
        m_registry.dropOwner(owner);
    }
    // Generic fallback banners live outside the registry; close them too so
    // no owner's content outlives the account on the desktop.
    for (const auto &tag : m_liveFallbackTags.take(owner)) {
        m_fallbackRoutes.remove(tag);
        if (m_transport)
            m_transport->closeByTag(tag);
    }
    const auto scope = owner + QStringLiteral("::");
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it.key().startsWith(scope)) it = m_pending.erase(it);
        else ++it;
    }
    QStringList keptOrder;
    for (const auto &key : m_processedOrder) {
        if (key.startsWith(scope)) m_processed.remove(key);
        else keptOrder.append(key);
    }
    m_processedOrder = keptOrder;
    m_fallbackRoutes.clear();
}

void NotificationService::onMessagesClosed(const QString &owner, const QList<QString> &messageUuids)
{
    if (!m_transport)
        return;
    for (const auto &messageUuid : messageUuids) {
        // A candidate that has not been shown yet must not surface after its
        // message was read or deleted (the reference re-checks before display).
        const auto key = owner + QStringLiteral("::") + messageUuid;
        if (m_pending.contains(key))
            m_pending.remove(key);
    }
    const auto outcome = m_registry.consumeRead(messageUuids, owner);
    for (const auto &tag : outcome.closedTags)
        m_transport->closeByTag(tag);
    for (const auto &messageUuid : outcome.untrackedMessageUuids) {
        const auto fallback = NotificationAggregateRegistry::fallbackTag(owner, messageUuid);
        m_liveFallbackTags[owner].remove(fallback);
        m_transport->closeByTag(fallback);
    }
}

void NotificationService::click(const QString &tag)
{
    if (const auto snap = m_registry.peek(tag)) {
        m_registry.consumeByTag(tag);
        if (m_transport)
            m_transport->closeByTag(tag);
        emit routeRequested(snap->routeStream, snap->routeTopic);
        return;
    }
    const auto route = m_fallbackRoutes.take(tag);
    if (route.first.isEmpty() && route.second.isEmpty())
        return;
    if (m_transport)
        m_transport->closeByTag(tag);
    emit routeRequested(route.first, route.second);
}

QString NotificationService::formatTitle(const NotificationTitleContext &context, int count)
{
    const auto mid = notificationSeparator();
    if (context.direct) {
        if (count <= 1)
            return clip200(context.senderName);
        const auto plural = count == 1 ? tr("%n message", "", count) : tr("%n messages", "", count);
        return clip200(plural + mid + context.conversationName);
    }
    const auto base = context.topicName.isEmpty()
        ? context.channelName
        : context.channelName + mid + context.topicName;
    if (count <= 1)
        return clip200(context.senderName + mid + base);
    const auto plural = count == 1 ? tr("%n message from %1", "", count)
                                   : tr("%n messages from %1", "", count);
    return clip200(plural.arg(context.senderName) + mid + base);
}

// ---------------------------------------------------------------------------
// Production transport
// ---------------------------------------------------------------------------

class DBusNotificationTransport::AttentionBackend {
public:
    explicit AttentionBackend(std::function<quintptr()> windowId)
        : m_windowId(std::move(windowId)) {}
    void flash(bool demand);

private:
    std::function<quintptr()> m_windowId;
#ifdef WORKSPACE_NATIVE_ATTENTION_XCB
    void *m_connection = nullptr; // xcb_connection_t*, kept opaque when XCB is off
#endif
};

#ifdef WORKSPACE_NATIVE_ATTENTION_XCB

void DBusNotificationTransport::AttentionBackend::flash(bool demand)
{
    const auto windowId = m_windowId ? m_windowId() : 0;
    if (windowId == 0)
        return;
    auto *connection = static_cast<xcb_connection_t *>(m_connection);
    if (connection == nullptr) {
        connection = xcb_connect(nullptr, nullptr);
        if (xcb_connection_has_error(connection)) {
            xcb_disconnect(connection);
            return;
        }
        m_connection = connection;
    }
    static xcb_atom_t netWmState = XCB_ATOM_NONE;
    static xcb_atom_t demandsAttention = XCB_ATOM_NONE;
    if (netWmState == XCB_ATOM_NONE) {
        const auto state = xcb_intern_atom(connection, 0, sizeof("_NET_WM_STATE") - 1, "_NET_WM_STATE");
        const auto attention = xcb_intern_atom(connection, 0,
            sizeof("_NET_WM_STATE_DEMANDS_ATTENTION") - 1, "_NET_WM_STATE_DEMANDS_ATTENTION");
        auto *stateReply = xcb_intern_atom_reply(connection, state, nullptr);
        auto *attentionReply = xcb_intern_atom_reply(connection, attention, nullptr);
        if (stateReply == nullptr || attentionReply == nullptr) {
            free(stateReply);
            free(attentionReply);
            return;
        }
        netWmState = stateReply->atom;
        demandsAttention = attentionReply->atom;
        free(stateReply);
        free(attentionReply);
    }
    const auto *setup = xcb_get_setup(connection);
    auto iterator = xcb_setup_roots_iterator(setup);
    const xcb_window_t root = iterator.data->root;
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = static_cast<xcb_window_t>(windowId);
    event.type = netWmState;
    event.data.data32[0] = demand ? 1 : 0; // _NET_WM_STATE_REMOVE=0 / _NET_WM_STATE_ADD=1
    event.data.data32[1] = demandsAttention;
    event.data.data32[2] = 1;             // source: application
    xcb_send_event(connection, 0, root,
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
        reinterpret_cast<const char *>(&event));
    xcb_flush(connection);
}

#else

void DBusNotificationTransport::AttentionBackend::flash(bool) {}

#endif

namespace {

void closeFreedesktop(uint id)
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || id == 0)
        return;
    auto call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("CloseNotification"));
    call.setArguments({id});
    bus.call(call, QDBus::NoBlock);
}

} // namespace

DBusNotificationTransport::DBusNotificationTransport(std::function<quintptr()> windowId,
                                                     QObject *parent)
    : QObject(parent), m_attention(std::make_unique<AttentionBackend>(std::move(windowId)))
{
}

DBusNotificationTransport::~DBusNotificationTransport() = default;

bool DBusNotificationTransport::show(const QString &tag, const QString &title, const QString &body)
{
    const auto clippedTitle = clip200(title);
    const auto clippedBody = clip200(body);
    if (clippedTitle.trimmed().isEmpty())
        return false;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    if (!m_connectionHooked) {
        m_connectionHooked = bus.connect(QStringLiteral("org.freedesktop.Notifications"),
            QStringLiteral("/org/freedesktop/Notifications"),
            QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("ActionInvoked"),
            this, SLOT(onActionInvoked(uint,QString)));
    }
    if (const auto previous = m_activeIds.take(tag); previous != 0) {
        m_tags.remove(previous);
        closeFreedesktop(previous);
    }
    auto call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    const auto appName = QCoreApplication::applicationName().isEmpty()
        ? QStringLiteral("Exordos Workspace") : QCoreApplication::applicationName();
    // org.freedesktop.Notifications.Notify(s app, u replaces_id, s icon,
    // s summary, s body, as actions, a{sv} hints, i expire_timeout). The
    // explicit default action guarantees ActionInvoked routing on servers
    // that do not synthesize it.
    call.setArguments({appName, QVariant::fromValue(quint32{0}), QString{}, clippedTitle,
        clippedBody, QStringList{QStringLiteral("default"), QString{}},
        QVariantMap{{QStringLiteral("suppress-sound"), true}}, -1});
    const auto reply = bus.call(call, QDBus::Block, 1000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return false;
    const auto id = reply.arguments().first().toUInt();
    if (id == 0)
        return false;
    m_activeIds.insert(tag, id);
    m_tags.insert(id, tag);
    return true;
}

void DBusNotificationTransport::closeByTag(const QString &tag)
{
    const auto id = m_activeIds.take(tag);
    if (id == 0)
        return;
    m_tags.remove(id);
    closeFreedesktop(id);
}

void DBusNotificationTransport::onActionInvoked(uint notificationId, const QString &)
{
    const auto tag = m_tags.value(notificationId);
    if (!tag.isEmpty() && m_click)
        m_click(tag);
}

void DBusNotificationTransport::requestAttention()
{
    m_attention->flash(true);
}

void DBusNotificationTransport::clearAttention()
{
    m_attention->flash(false);
}

} // namespace workspace
