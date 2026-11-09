#pragma once

#include "native-draft-store.h"
#include <QObject>
#include <memory>
#include <functional>

namespace workspace {

// GUI-facing, bounded storage worker. Admission is not durable completion.
// Accepted writes retain their captured owner through account/route changes.
class NativeDraftService final : public QObject {
    Q_OBJECT
public:
    using Failure = NativeDraftStore::Failure;
    struct Lease { QString owner; quint64 issuer = 0, generation = 0; };
    using Done = std::function<void(Failure)>;
    using ReadDone = std::function<void(NativeDraftStore::Read)>;
    using PageDone = std::function<void(NativeDraftStore::Page)>;
    using PreviewDone = std::function<void(NativeDraftStore::PreviewPage)>;
    static constexpr int MaximumDataJobs = 8;
    static constexpr int MaximumControlJobs = 2; // Stop has one reserved barrier.
    static constexpr qsizetype MaximumPendingBytes = 32 * 1024 * 1024;

    explicit NativeDraftService(QString directory = {}, QObject *parent = nullptr);
    ~NativeDraftService() override;
    bool open(const QString &owner);
    void close();
    Lease lease() const;
    bool available() const;
    Failure failure() const;
    bool busy() const;
    int pendingDataJobs() const;
    qsizetype pendingBytes() const;
    bool read(const Lease &lease, const QString &uuid, QObject *receiver, ReadDone done);
    bool latest(const Lease &lease, const QString &stream, const QString &topic, QObject *receiver, ReadDone done);
    bool list(const Lease &lease, const QString &afterUuid, QObject *receiver, PageDone done);
    bool previews(const Lease &lease, const QString &after, QObject *receiver, PreviewDone done);
    bool apply(const Lease &lease, const QVector<DraftRecord> &writes, const QStringList &removals,
               QObject *receiver = nullptr, Done done = {});
    bool retainStreams(const Lease &lease, const QSet<QString> &authorized,
                       QObject *receiver = nullptr, Done done = {});
    // FIFO completion barrier. Per-Apply receipts report durable success/failure.
    bool flush(QObject *receiver, Done done);
    void shutdown();
    bool stopped() const;

signals:
    void stateChanged();
    void shutdownFinished();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace workspace
