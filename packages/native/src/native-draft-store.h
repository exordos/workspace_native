#pragma once

#include <QJsonObject>
#include <QSqlDatabase>
#include <QStringList>
#include <QVector>
#include <memory>
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
    Failure put(const DraftRecord &record);
    Failure remove(const QString &uuid);
    Failure apply(const QVector<DraftRecord> &writes, const QStringList &removals);
    static bool valid(const DraftRecord &record);

private:
    bool sameThread() const;
    bool protectFiles();
    QString m_directory, m_connection, m_path;
    QThread *m_thread;
    QSqlDatabase m_database;
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace workspace
