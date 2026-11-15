#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QStringList>
#include <QVector>
#include <QSet>
#include <memory>
#include <mutex>
#include <optional>

class QLockFile;
class QThread;

namespace workspace {

struct DraftRecord {
    enum class Phase { Local, Saving, Saved, Failed, Conflict, Deleting };
    QString uuid, stream, topic, snapshot, text;
    QJsonObject replySession;
    qint64 updatedAt = 0;
    qint64 revision = 0;
    Phase phase = Phase::Local;
    bool consumed = false;
    // Freeze the first POST independently of later input until its outcome is known.
    bool createPending = false;
    QString createText;
    QString conflictText;
    qint64 conflictRevision = 0;
    // Server time is independent of the local editor's millisecond ordering.
    QString serverUpdatedAt;
    // A public draft promoted to private input retains only deletion metadata.
    bool privateDeletion = false;
    bool privateCreateUncertain = false;
};

// Immutable pre-dispatch composer intent. Encrypted entries contain ciphertext
// only; private composer text and key bytes never enter this store.
struct OutgoingRecord {
    QString uuid, stream, topic, routeTopic, publicId;
    QJsonObject payload;
    QString draftUuid, draftSnapshot, draftText;
    QString replyActive;
    QJsonArray replySnapshot;
    qint64 createdAt = 0, deletionRevision = 0;
};

// One controller and its existing storage worker order deletion observation
// against retirement. SQL statement/busy waits run outside this short guard;
// only the final WAL commit shares the deletion boundary.
struct OutgoingRetirement {
    std::mutex mutex;
    bool cancelled = false;
    void cancel() { const std::lock_guard<std::mutex> lock(mutex); cancelled = true; }
};

// Durable unsent work is separate from the evictable history cache. This
// synchronous backend is constructed, used and destroyed on one storage worker;
// the GUI coordinator must debounce/coalesce writes before admission.
class NativeDraftStore final {
public:
    enum class Failure { None, InvalidOwner, InvalidRecord, Capacity, Unavailable, Locked, Corrupt, Schema };
    struct Summary {
        QString uuid, stream, topic;
        qint64 updatedAt = 0;
        DraftRecord::Phase phase = DraftRecord::Phase::Local;
        bool consumed = false;
    };
    struct Page {
        Failure failure = Failure::Unavailable;
        QVector<Summary> rows;
        QString next;
    };
    struct Read {
        Failure failure = Failure::Unavailable;
        std::optional<DraftRecord> record;
    };
    struct Preview {
        QString uuid, stream, topic, snapshot, text;
        qint64 updatedAt = 0;
        DraftRecord::Phase phase = DraftRecord::Phase::Local;
        bool consumed = false;
    };
    struct PreviewPage {
        Failure failure = Failure::Unavailable;
        QVector<Preview> rows;
        QString next;
    };
    struct OutgoingPage {
        Failure failure = Failure::Unavailable;
        QVector<OutgoingRecord> rows;
    };
    static constexpr int MaximumOutgoingRecords = 64;
    static constexpr qsizetype MaximumOutgoingBytes = 16 * 1024 * 1024;
    static constexpr qsizetype MaximumOutgoingRecordBytes = 512 * 1024;
    static bool valid(const OutgoingRecord &record);
    static qsizetype serializedSize(const OutgoingRecord &record);
    OutgoingPage outgoing();
    Failure putOutgoing(const OutgoingRecord &record);
    // Atomically retire the send and mark only its captured draft consumed.
    Failure finishOutgoing(const QString &uuid, bool consumeDraft,
                           const std::shared_ptr<OutgoingRetirement> &retirement = {});
    Failure clearOutgoing();
    static constexpr qsizetype MaximumRecordBytes = 4 * 1024 * 1024;
    static constexpr int MaximumPageRows = 100;
    static constexpr qsizetype MaximumBatchBytes = 8 * 1024 * 1024;

    explicit NativeDraftStore(QString directory = {});
    ~NativeDraftStore();
    NativeDraftStore(const NativeDraftStore &) = delete;
    NativeDraftStore &operator=(const NativeDraftStore &) = delete;
    Failure open(const QString &owner);
    void close();
    bool available() const;
    Page list(const QString &afterUuid = {}, int limit = MaximumPageRows);
    Read read(const QString &uuid);
    Read latest(const QString &stream, const QString &topic);
    PreviewPage previews(const QString &after = {});
    Failure put(const DraftRecord &record);
    Failure remove(const QString &uuid);
    Failure apply(const QVector<DraftRecord> &writes, const QStringList &removals);
    Failure retainStreams(const QSet<QString> &authorized);
    static bool valid(const DraftRecord &record);
    static qsizetype serializedSize(const DraftRecord &record);

private:
    bool sameThread() const;
    bool protectFiles();
    QString m_directory, m_connection, m_path;
    QThread *m_thread;
    QSqlDatabase m_database;
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace workspace
