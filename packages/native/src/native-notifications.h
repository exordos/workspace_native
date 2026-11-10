#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

namespace workspace {

// Desktop notification pipeline for the native client, ported from the
// reference contract at workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af
// (docs/NATIVE_DESKTOP_NOTIFICATIONS.md). Trigger classification and mute
// rules mirror shared/lib/workspace-desktop-notifications.lib.ts; the
// aggregate/tag scheme mirrors notification-aggregate-registry.lib.ts and
// layout-notification-tag.lib.ts; dedup mirrors
// shared/lib/notification-dedup.lib.ts.

// The shell seam. Production is the session-bus org.freedesktop.Notifications
// transport plus the X11 attention adapter; tests inject a recording fake.
class NotificationTransport {
public:
    virtual ~NotificationTransport() = default;
    // One active notification per tag: callers close the previous holder
    // before showing (Electron notifications:show does the same).
    virtual bool show(const QString &tag, const QString &title, const QString &body) = 0;
    virtual void closeByTag(const QString &tag) = 0;
    virtual void requestAttention() = 0;
    virtual void clearAttention() = 0;
};

struct NotificationViewport {
    bool windowFocused = true;
    bool conversationOnScreen = false;
};

struct NotificationCandidate {
    QString owner;
    QString messageUuid;
    QString streamUuid;
    QString topicUuid;
    QString authorUuid;
    QString content;
    bool isOwn = false;
    bool read = false;
    // Server DTO tri-state collapsed: unknown counts as "no mention" exactly
    // like the reference projection signal (native never parses mention entities).
    bool mentioned = false;
    qint64 createdMicros = 0;
    // Metadata resolution results, filled by the resolver callback.
    bool streamKnown = false;
    bool topicKnown = false;
    bool streamPrivate = false;
    QString streamName;
    QString topicName;
    QString topicMode;      // "default" | "follow" | "mute" | "" unknown
    QString streamMode;     // "all_messages" | "mentions_only" | "muted" | "" unknown
    QString senderName;
    // Viewport facts for this candidate's conversation.
    NotificationViewport viewport;
};

struct NotificationDecision {
    bool notify = false;
    QString trigger;
};

// Policy layer (pure).
namespace notification_policy {

QString wildcardMentionCheck(const QString &content);
QString classifyTrigger(const NotificationCandidate &candidate);
NotificationDecision shouldNotify(const NotificationCandidate &candidate, bool metadataUnavailable);

} // namespace notification_policy

// The reference joins notification title segments and the generic body with
// the middle dot U+00B7 (layout-notification-title.lib.ts).
QString notificationSeparator();

// Aggregate registry: bucket keys, tags, per-tag message sets and the
// read-close semantics of closeReadMessageNotifications.
struct NotificationTitleContext {
    bool direct = false;         // dm bucket
    QString senderName;
    QString conversationName;    // dm conversation name
    QString channelName;         // stream name for channel buckets
    QString topicName;           // empty when default/unknown topic
};

struct NotificationSnapshot {
    QString tag;
    int count = 0;
    QString lastMessageUuid;
    QString body;
    QString routeStream;
    QString routeTopic;
    NotificationTitleContext titleContext;
};

struct NotificationReadOutcome {
    QStringList closedTags;
    QStringList untrackedMessageUuids;
};

class NotificationAggregateRegistry {
public:
    static QString aggregateTag(const QString &owner, const QString &bucketKey);
    static QString fallbackTag(const QString &owner, const QString &messageUuid);
    static QString bucketKeyFor(const NotificationCandidate &candidate);
    std::optional<NotificationSnapshot> peek(const QString &tag) const;

    std::optional<NotificationSnapshot> upsert(const NotificationCandidate &candidate,
                                               const NotificationTitleContext &title);
    QList<QString> consumeByTag(const QString &tag);
    NotificationReadOutcome consumeRead(const QList<QString> &messageUuids, const QString &owner);
    QStringList dropOwner(const QString &owner);
    int tagCount() const { return int(m_byTag.size()); }
    int rowCount(const QString &tag) const
    {
        const auto it = m_byTag.constFind(tag);
        return it == m_byTag.cend() ? -1 : int(it->messages.size());
    }
    // Tags dropped by the aggregate bound; the service closes their banners.
    QStringList takeEvicted()
    {
        QStringList out;
        out.swap(m_evicted);
        return out;
    }
    int count(const QString &tag) const;
    QStringList tags() const;

private:
    QStringList m_evicted; // tags removed by the aggregate bound
    struct Entry {
        struct Row {
            QString body;
            QString routeStream;
            QString routeTopic;
            NotificationTitleContext title;
            qint64 createdMicros = 0;
        };
        QHash<QString, Row> messages;
        qint64 total = 0;
    };
    std::optional<NotificationSnapshot> snapshot(const QString &tag, const Entry &entry) const;

    QHash<QString, Entry> m_byTag;
    QHash<QString, QString> m_messageToTag; // "owner::message:uuid" -> tag
};

// Recent owner::messageUuid dedup, capped exactly like the reference.
class NotificationDedup {
public:
    bool contains(const QString &owner, const QString &messageUuid) const;
    void insert(const QString &owner, const QString &messageUuid);
    void clear();

private:
    static constexpr int kMaxRecentKeys = 200; // notification-dedup.lib.ts MAX_RECENT_KEYS
    QList<QString> m_order;
    QSet<QString> m_keys;
};

// The orchestrator owned by NativeController. Candidates arrive from
// background realtime messages; the service decides, aggregates and shows.
class NotificationService final : public QObject {
    Q_OBJECT
public:
    explicit NotificationService(QObject *parent = nullptr);

    void setTransport(NotificationTransport *transport) { m_transport = transport; }
    NotificationTransport *transport() const { return m_transport; }
    // Fills metadata fields of the candidate and returns true when the
    // conversation metadata needed for a named notification is available.
    void setMetadataResolver(std::function<bool(NotificationCandidate &)> resolver)
    { m_resolver = std::move(resolver); }
    void setGenericBody(std::function<QString()> provider) { m_genericBody = std::move(provider); }
    // Test seam for the reference 30 s grace / 250 ms retry.
    void setMetadataGrace(int graceMs, int retryMs);

    void setWindowFocused(bool focused);
    bool windowFocused() const { return m_windowFocused; }

    void ingest(const NotificationCandidate &candidate);
    void onMessagesClosed(const QString &owner, const QList<QString> &messageUuids);
    // Account transition: close every banner the owner still has open and
    // drop its pending/pending-decision state (closeAllActiveMessageNotifications).
    void onOwnerRetired(const QString &owner);
    void click(const QString &tag);

    // Observability for tests and future diagnostics.
    int aggregateCount(const QString &tag) const { return m_registry.count(tag); }
    int aggregateRowCount(const QString &tag) const { return m_registry.rowCount(tag); }
    int processedCount() const { return int(m_processed.size()); }
    int pendingCount() const { return int(m_pending.size()); }
    int trackedTagCount() const { return m_registry.tagCount(); }
    QStringList activeTags() const { return m_registry.tags(); }
    static QString formatTitle(const NotificationTitleContext &context, int count);
    static QString fallbackTagFor(const QString &owner, const QString &messageUuid)
    { return NotificationAggregateRegistry::fallbackTag(owner, messageUuid); }

signals:
    // A notification click was routed: open the conversation (topic wins;
    // a dm falls back to the stream conversation).
    void routeRequested(const QString &streamUuid, const QString &topicUuid);

private:
    struct Pending {
        NotificationCandidate candidate;
        qint64 observedMs = 0;
    };

    void ingestNow(const NotificationCandidate &candidate);
    void markProcessed(const QString &key);
    void showGeneric(const NotificationCandidate &candidate);
    void retryPending();
    void scheduleGraceRetry();
    static qint64 nowMs();

    NotificationTransport *m_transport = nullptr;
    std::function<bool(NotificationCandidate &)> m_resolver;
    std::function<QString()> m_genericBody;
    NotificationAggregateRegistry m_registry;
    NotificationDedup m_dedup;
    QSet<QString> m_processed;         // owner::messageUuid decisions
    QStringList m_processedOrder;      // insertion order for eviction
    QHash<QString, QPair<QString, QString>> m_fallbackRoutes; // tag -> (stream, topic)
    // Generic (msg: scheme) banners are outside the aggregate registry; the
    // service tracks them per owner so retirement can close them too.
    QHash<QString, QSet<QString>> m_liveFallbackTags;
    QHash<QString, Pending> m_pending;
    QTimer m_graceTimer;
    int m_graceMs = 30000;   // NOTIFICATION_METADATA_GRACE_MS
    int m_retryMs = 250;     // NOTIFICATION_METADATA_RETRY_MS
    bool m_windowFocused = true;
};

// Session-bus transport (org.freedesktop.Notifications) + X11 attention.
// show()/closeByTag() truncate to 200 characters and reject empty titles
// exactly like packages/electron/src/main.ts notifications:show.
class DBusNotificationTransport final : public QObject, public NotificationTransport {
    Q_OBJECT
public:
    explicit DBusNotificationTransport(std::function<quintptr()> windowId = {},
                                       QObject *parent = nullptr);
    ~DBusNotificationTransport() override;

    void setClickCallback(std::function<void(const QString &tag)> callback)
    { m_click = std::move(callback); }

    bool show(const QString &tag, const QString &title, const QString &body) override;
    void closeByTag(const QString &tag) override;
    void requestAttention() override;
    void clearAttention() override;

private slots:
    // org.freedesktop.Notifications ActionInvoked (id, action).
    void onActionInvoked(uint notificationId, const QString &action);

private:
    class AttentionBackend;
    std::unique_ptr<AttentionBackend> m_attention;
    QHash<QString, quint32> m_activeIds; // tag -> notification id
    QHash<quint32, QString> m_tags;      // notification id -> tag
    std::function<void(const QString &tag)> m_click;
    bool m_connectionHooked = false;
};

} // namespace workspace
