#pragma once

#include "session-vault.h"
#include <QSqlDatabase>
#include <QVector>
#include <deque>
#include <memory>
#include <optional>

class QLockFile;

namespace workspace {

// Dedicated durable secrets, separate from the evictable messenger cache.
// Device/generation keys, pending requests and atomic completion receipts.
class NativeChatKeyVault final : public QObject {
    Q_OBJECT
public:
    enum class Failure {
        None, InvalidOwner, InvalidAddress, InvalidKey, Unavailable, Denied,
        MissingMasterKey, Corrupt, Superseded, Conflict, Capacity, MissingRequest, MissingKey
    };
    struct Lease { QString owner; QString device; quint64 issuer = 0; quint64 generation = 0; };
    struct Device { QString id; QString name; };
    struct Address { QString stream; QString key; };
    struct Key {
        QByteArray bytes;
        quint64 deletionRevision = 0;
        qint64 receivedAt = 0;
        quint64 sendOrder = 0;
    };
    struct ReadResult { Failure failure = Failure::Unavailable; Key value; };
    struct KeyMetadata { QString key; qint64 receivedAt = 0; };
    struct MetadataResult { Failure failure = Failure::Unavailable; QVector<KeyMetadata> values; };
    struct AvailableKeyResult { Failure failure = Failure::Unavailable; QString key; Key value; };
    struct Recovery { QByteArray envelope; QByteArray previousKey; };
    struct PendingRequest {
        QString localId;
        QString messageId;
        QString deviceName;
        QByteArray publicKey;
        QByteArray privateKey;
        qint64 createdAt = 0;
        quint64 deletionRevision = 0;
        std::optional<Recovery> recovery;
        QString targetTopic;
        void clearSecrets();
    };
    enum class Outcome { Granted, Rejected };
    struct Completion { QString localId; QString requestMessage; QString responseMessage; Outcome outcome = Outcome::Rejected; };
    struct PendingResult { Failure failure = Failure::Unavailable; QVector<PendingRequest> values; };
    struct CompletionResult { Failure failure = Failure::Unavailable; std::optional<Completion> value; };
    // Durable pre-dispatch record for an encrypted channel creation: the group
    // key plus the identities needed to recover the attempt after a restart
    // (the idempotent stream-creation uuid and the generation uuid the server
    // echoes). Written before the stream POST and cleared only after the
    // canonical echo verified and the key committed.
    struct CreateStage {
        QString attempt;
        QString key;
        QByteArray groupKey;
        qint64 createdAt = 0;
        void clearSecrets();
    };
    struct CreateStageResult { Failure failure = Failure::Unavailable; std::optional<CreateStage> value; };
    struct CreateStageListResult { Failure failure = Failure::Unavailable; QVector<CreateStage> values; };
    using Opened = std::function<void(Failure)>;

    explicit NativeChatKeyVault(QString directory = {}, SessionSecretStore *backend = nullptr,
                                QObject *parent = nullptr);
    ~NativeChatKeyVault() override;
    void open(const QUrl &server, const QString &project, const QString &user,
              const QString &deviceName, Opened done);
    void close();
    bool available() const { return m_database.isOpen() && m_master.size() == 32; }
    Lease lease() const;
    Device device() const { return available() ? m_device : Device{}; }
    ReadResult read(const Lease &lease, const Address &address);
    Failure put(const Lease &lease, const Address &address, const QByteArray &key,
                quint64 expectedDeletionRevision);
    Failure remove(const Lease &lease, const Address &address);
    MetadataResult listKeys(const Lease &lease, const QString &stream);
    AvailableKeyResult lastAvailableKey(const Lease &lease, const QString &stream);
    Failure markForSending(const Lease &lease, const Address &address, quint64 expectedDeletionRevision);
    PendingResult pendingRequests(const Lease &lease, const Address &address);
    Failure putPendingRequest(const Lease &lease, const Address &address, const PendingRequest &request);
    Failure bindPendingRequest(const Lease &lease, const Address &address,
                               const QString &localId, const QString &messageId);
    CompletionResult requestCompletion(const Lease &lease, const Address &address, const QString &localId);
    CompletionResult requestCompletionByMessage(const Lease &lease, const Address &address, const QString &messageId);
    Failure completeRequest(const Lease &lease, const Address &address, const Completion &completion,
                            const QByteArray &groupKey = {}, bool replaceExisting = false);
    Failure putCreateStage(const Lease &lease, const CreateStage &stage);
    CreateStageResult readCreateStage(const Lease &lease, const QString &attempt);
    CreateStageListResult createStages(const Lease &lease);
    Failure clearCreateStage(const Lease &lease, const QString &attempt);

    static constexpr qsizetype MaximumRecordBytes = 2048;
    static constexpr qint64 MaximumDatabaseBytes = 64 * 1024 * 1024;
    static constexpr qsizetype MaximumPendingRequests = 32;
    static constexpr qsizetype MaximumRequestRecordBytes = 1024 * 1024;
    static constexpr qsizetype MaximumRecoveryEnvelopeBytes = 512 * 1024;
    static constexpr qint64 MaximumPendingBytes = 8 * 1024 * 1024;
    static constexpr qsizetype MaximumCreateStages = 32;

signals:
    // Material/deletion changes are published only after their transaction commits.
    // Neither keys nor encrypted records are exposed by these notifications.
    void keyMaterialChanged(const QString &stream, const QString &key);
    void stateChanged();

private:
    bool accepts(const Lease &lease) const;
    Failure initialize(const QString &path, const QString &deviceName);
    Failure writeRecord(const QString &address, const QByteArray &value, qsizetype limit = MaximumRecordBytes);
    std::optional<QByteArray> readRecord(const QString &address, Failure &failure, qsizetype limit = MaximumRecordBytes);
    ReadResult readKey(const Address &address);
    Failure visitStreamKeys(const QString &stream, const std::function<Failure(const QString &, const Key &)> &visit);
    Failure writeKey(const Address &address, const Key &key);
    QString addressKey(const Address &address) const;
    QString requestPrefix(const char *type, const Address &address) const;
    QString requestAddress(const char *type, const Address &address, const QString &id) const;
    std::optional<PendingRequest> readPending(const Address &address, const QString &id, Failure &failure);
    CompletionResult readCompletion(const QString &record);
    Failure requestCapacity(const Address &address, const QString &record, qsizetype encodedBytes);
    QString createStagePrefix() const;
    QString createStageAddress(const QString &attempt) const;
    std::optional<CreateStage> readCreateStageRecord(const QString &attempt, Failure &failure);
    Failure finishTransaction(Failure result);
    void resetStorage();
    void startNextOpen();
    void finishOpen(Opened done, Failure failure);

    QString m_directory;
    QPointer<SessionSecretStore> m_backend;
    QSqlDatabase m_database;
    QString m_connection;
    QString m_owner;
    QByteArray m_master;
    Device m_device;
    quint64 m_issuer = 0;
    quint64 m_generation = 0;
    std::unique_ptr<QLockFile> m_lock;
    std::deque<std::function<void()>> m_openQueue;
    bool m_openActive = false;
    quint64 m_activeGeneration = 0;
    Opened m_activeDone;
};

} // namespace workspace
