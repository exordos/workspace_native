#pragma once

#include "native-cache.h"

#include <QPointer>
#include <memory>

namespace workspace
{

// GUI-facing cache coordinator. All SQLite and filesystem work belongs to its
// single worker; accepted operations are not claims of durable completion.
class NativeCacheService final : public QObject
{
    Q_OBJECT
  public:
    using Kind = NativeCache::Kind;
    using Key = NativeCache::Key;
    using Entry = NativeCache::Entry;
    using MessageBody = NativeCache::MessageBody;
    using Lease = NativeCache::Lease;
    using Policy = NativeCache::Policy;
    using Stats = NativeCache::Stats;
    using Projection = NativeCache::Projection;
    using ProjectionFields = NativeCache::ProjectionFields;
    using Completion = std::function<void(bool)>;
    using ReadCallback = std::function<void(std::optional<Entry>)>;
    using BodyReadCallback = std::function<void(std::optional<MessageBody>)>;
    static constexpr int MaximumQueuedJobs = 8;
    static constexpr qint64 MaximumQueuedBytes = 32 * 1024 * 1024;
    static constexpr qint64 MaximumPageBytes = 8 * 1024 * 1024;

    explicit NativeCacheService(QString directory = {}, QObject *parent = nullptr);
    NativeCacheService(QString directory, Policy policy, bool allowLoopbackForTests,
                       std::function<qint64()> clock = {}, QObject *parent = nullptr);
    ~NativeCacheService() override;

    // A lease is minted immediately, but reads bypass the cache until ready.
    bool open(const QUrl &server, const QString &project, const QString &user, QObject *receiver = nullptr,
              Completion done = {});
    void close();
    Lease lease() const;
    bool available() const;
    QString error() const;
    Stats stats() const;
    qint64 diskUsageBytes() const;
    qint64 requestedDiskLimitBytes() const;
    qint64 appliedDiskLimitBytes() const;
    bool maintenanceBusy() const;
    // True means validation/acceptance. limitApplied is emitted only after the
    // actual DB+WAL+SHM fits; reductions can remain pending in the background.
    bool setDiskLimitBytes(qint64 bytes);

    void get(const Lease &lease, const Key &key, QObject *receiver, ReadCallback done);
    void getMessageBody(const Lease &lease, const QString &uuid, QObject *receiver, BodyReadCallback done);
    bool putMessageBody(const Lease &lease, const QJsonObject &canonical,
                        QObject *receiver = nullptr, Completion done = {});
    // Captured-owner required erasure, not an optional write. Accepted means
    // queued; only done(true) confirms Body and all indexed History are erased.
    bool removeMessageBodyAndHistory(const Lease &lease, const QString &uuid,
                                     QObject *receiver = nullptr, Completion done = {});
    // Local projections stay saved but cannot become authoritative warm pages.
    // The retained write and freshness change share one optional worker job.
    bool put(const Lease &lease, const Key &key, const QJsonArray &rows, const QByteArray &nextMarker = {},
             QObject *receiver = nullptr, Completion done = {}, bool retainOnly = false);
    bool invalidate(const Lease &lease, Kind kind, const QString &stream = {}, const QString &topic = {},
                    QObject *receiver = nullptr, Completion done = {});
    void markStale(const Lease &lease);
    bool markStale(const Lease &lease, Kind kind, const QString &stream = {}, const QString &topic = {});
    bool markHistoryRead(const Lease &lease, const QString &stream, const QString &topic,
                         const QSet<QString> &ids, QObject *receiver = nullptr, Completion done = {});
    bool projectMessage(const Lease &lease, const QJsonObject &canonical, ProjectionFields fields,
                        QObject *receiver = nullptr, Completion done = {});
    bool clearOwner(const Lease &lease, QObject *receiver = nullptr, Completion done = {});
    bool forgetAccount(const QUrl &server, const QString &project, const QString &user,
                       QObject *receiver = nullptr, Completion done = {});
    void flush(QObject *receiver, Completion done);
    // Stop admission, cancel maintenance, drain accepted bounded writes and
    // required erasures, then close/remove the SQL connection on the worker.
    void shutdown();
    bool stopped() const;

  signals:
    void stateChanged();
    void errorChanged();
    void limitApplied(qint64 bytes);
    void shutdownFinished();

  private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace workspace
