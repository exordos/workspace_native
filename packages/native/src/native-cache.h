#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QUrl>
#include <functional>
#include <optional>

namespace workspace {

// A bounded response cache, never an authentication or mutation-confirmation
// source. Open only after Session has verified this exact owner.
class NativeCache final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 DefaultDiskLimitBytes = 10000000000LL;
    static constexpr qint64 MinimumDiskLimitBytes = 1024 * 1024;
    static constexpr qint64 MaximumDiskLimitBytes = 1000000000000LL;
    static constexpr int MaximumFreshPages = 4096;
    enum class Kind { Streams = 1, Users, Topics, History, MessageBody };
    enum class Projection { Insert = 1, Payload = 2, Star = 4, Reactions = 8 };
    Q_DECLARE_FLAGS(ProjectionFields, Projection)
    struct Lease {
        QString owner;
        quint64 generation = 0;
    };
    struct Key {
        Kind kind;
        QString stream;
        QString topic;
        QByteArray marker;
    };
    struct Entry {
        QJsonArray rows;
        QByteArray nextMarker;
        qint64 savedAt = 0;
        bool fresh = false;
    };
    struct MessageBody {
        QJsonObject row;
        qint64 savedAt = 0;
    };
    static constexpr int MaximumHistoryBodyCandidates = 4;
    struct Policy {
        int maxEntries = 0; // zero means the physical disk budget determines retention
        qint64 maxBytes = MaximumDiskLimitBytes;
        qint64 maxEntryBytes = 8 * 1024 * 1024;
        qint64 ttlMs = 120000;
    };
    struct Stats {
        qint64 entries = 0;
        qint64 bytes = 0;
        quint64 hits = 0;
        quint64 staleHits = 0;
        quint64 misses = 0;
        int freshPages = 0;
        quint64 fullScans = 0;
        quint64 checkpoints = 0;
        qint64 sampledPeakDiskBytes = 0;
    };
    explicit NativeCache(QString directory = {}, QObject *parent = nullptr);
    NativeCache(QString directory, Policy policy, bool allowLoopbackForTests,
                std::function<qint64()> clock = {}, QObject *parent = nullptr);
    ~NativeCache() override;
    bool open(const QUrl &server, const QString &project, const QString &user);
    void close();
    Lease lease() const { return {m_owner, m_generation}; }
    bool available() const { return m_database.isOpen() && !m_owner.isEmpty(); }
    QString error() const { return m_error; }
    qint64 diskLimitBytes() const { return m_diskLimitBytes; }
    qint64 diskUsageBytes() const;
    bool setDiskLimitBytes(qint64 bytes);
    // Worker-owned operations poll between bounded SQL/row/vacuum steps. The
    // callback must be thread-safe and must not touch GUI objects.
    void setCancellationCheck(std::function<bool()> check) { m_cancel = std::move(check); }
    std::optional<Entry> get(const Lease &lease, const Key &key);
    bool put(const Lease &lease, const Key &key, const QJsonArray &rows,
             const QByteArray &nextMarker = {});
    // A body is owner+UUID-addressed data, never a complete history page or
    // proof of current permission. Reads try at most four indexed History pages
    // after the exact Body key. Legacy pages without viewer identity cannot
    // supply an owned body. No payload scan or body reservoir is used.
    std::optional<MessageBody> getMessageBody(const Lease &lease, const QString &uuid);
    bool putMessageBody(const Lease &lease, const QJsonObject &canonical);
    // Authoritative successful omission/deletion only, not HTTP/transport error.
    // Erases the Body and all indexed History copies for this exact owner+UUID.
    bool removeMessageBodyAndHistory(const Lease &lease, const QString &uuid);
    // Delete affected pages, including pagination variants. Empty route fields
    // select all routes of that kind for this owner only.
    // Empty topic is an exact aggregate key for get/put, but an existing
    // wildcard in invalidation selectors. A topic change also affects aggregate.
    static bool matchesScope(const Key &key, Kind kind, const QString &stream = {}, const QString &topic = {});
    bool invalidate(const Lease &lease, Kind kind,
                    const QString &stream = {}, const QString &topic = {});
    // Disconnect/full baseline: cached rows may hydrate, but must revalidate.
    void markStale(const Lease &lease);
    // Retain the last validated response for offline display without retaining
    // permission to reuse it as a current projection.
    bool markStale(const Lease &lease, Kind kind,
                   const QString &stream = {}, const QString &topic = {});
    // After an authoritative read acknowledgement, patch only these already
    // stored rows' monotonic read flags. Payloads, cursors and ages are retained;
    // affected pages remain stale. Empty route fields select this owner only.
    bool markHistoryRead(const Lease &lease, const QString &stream,
                         const QString &topic, const QSet<QString> &confirmedIds);
    // Apply only fields from a verified raw GET/event DTO to already saved
    // history. Never creates a complete-looking page, refreshes its age/cursor,
    // reconstructs encoded payloads, or restores a deleted page.
    bool projectMessage(const Lease &lease, const QJsonObject &canonical, ProjectionFields fields);
    bool clearOwner(const Lease &lease);
    // Cleanup only: accept the exact identity of a verified saved account being
    // forgotten. Does not expose its cached rows or switch the active owner.
    bool forgetAccount(const QUrl &server, const QString &project, const QString &user);
    Stats stats() const;

signals:
    void errorChanged();

private:
    bool accepts(const Lease &lease) const;
    QByteArray keyHash(const Key &key) const;
    std::optional<QJsonArray> sanitize(const Key &key, const QJsonArray &rows) const;
    bool fail();
    bool trim(qint64 incomingBytes = 0);
    bool checkpoint(bool enforceLimit = true);
    bool applyDiskLimit();
    qint64 databaseLimitBytes() const;
    qint64 entryLimitBytes() const;
    bool indexHistory(const QString &owner, const QByteArray &key, const QJsonArray &rows);
    bool refreshTotals(bool force = false) const;
    bool eraseMatching(const QString &where, const QVariantList &bindings);
    void setError(const QString &message);
    bool shouldCancel() const { return m_cancel && m_cancel(); }
    QString m_directory;
    Policy m_policy;
    bool m_allowLoopback = false;
    std::function<qint64()> m_clock;
    std::function<bool()> m_cancel;
    QSqlDatabase m_database;
    QString m_connection;
    QString m_owner;
    QString m_project;
    QString m_user;
    QString m_error;
    quint64 m_generation = 0;
    mutable qint64 m_accessSequence = 0;
    mutable QHash<QByteArray, Key> m_fresh;
    mutable Stats m_stats;
    qint64 m_diskLimitBytes = DefaultDiskLimitBytes;
    mutable qint64 m_totalEntries = 0;
    mutable qint64 m_totalBytes = 0;
    mutable qint64 m_dataVersion = -1;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(NativeCache::ProjectionFields)

} // namespace workspace
