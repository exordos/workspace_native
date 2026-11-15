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
    //
    // Two-phase dispatch contract (review findings 4575672241/4575672242):
    // a stage starts in phase 'prepared' (dispatchAttempted == false). The
    // controller durably marks it 'dispatch-attempted' (markCreateStage-
    // DispatchAttempted) immediately before the stream POST leaves the
    // process. Recovery may only retire a 'prepared' stage whose POST can
    // therefore provably never have been dispatched; a dispatch-attempted
    // stage may still land server-side and is retained on inconclusive
    // answers. Records written before this contract existed (no field) decode
    // as dispatch-attempted: the safe, never-lossy default.
    //
    // Dormant quarantine (review findings 4575672241/4575672242 as
    // re-scoped by 4576535469): a dispatch-attempted stage is retained on
    // non-committal answers, and a definitive 404 proves only that the
    // stream is absent NOW — never that the dispatched POST can no longer
    // land (this client observes no server-side deadline or cancellation
    // for the attempt), so absence alone is not a definitive terminal
    // outcome and may never destroy the stage's group key. Each recovery
    // scan cycle that observes a DEFINITIVE 404 durably bumps
    // missingEvidence (a 2xx answer resets it to 0; transport errors and
    // 5xx prove neither and leave it untouched); the counter SATURATES at
    // the controller's minimumChannelStageMissingObservations. A stage at
    // saturation is DORMANT: its key stays quarantined inside this same
    // bounded MaximumCreateStages budget — dormant records are stage
    // records, never extra storage and never evicted — recovery keeps
    // re-checking them with its bounded backoff, and when a stream finally
    // lands the verified encrypted echo commits the key and frees the
    // slot (re-adoption). The residual starvation bound accepted by
    // design: a full budget of dormant attempts rejects new encrypted
    // creates locally until one of their streams lands; the alternative —
    // evicting dormant keys to free slots — would restore the very key
    // loss that finding 4576535469 reported.
    struct CreateStage {
        QString attempt;
        QString key;
        QByteArray groupKey;
        qint64 createdAt = 0;
        bool dispatchAttempted = false;
        int missingEvidence = 0;
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
    // Durably transition a stored stage from phase 'prepared' to phase
    // 'dispatch-attempted'. Must be called by the creator BEFORE the stream
    // POST is handed to the network stack; idempotent. A stage that stays
    // 'prepared' across a restart proves its POST never left the process, so
    // recovery may retire it without risking a channel whose key is only in
    // the vault. Missing stage: MissingKey; lease mismatch: Superseded.
    Failure markCreateStageDispatchAttempted(const Lease &lease, const QString &attempt);
    // Durably store the controller's conclusive-absence evidence for a stage
    // (see CreateStage::missingEvidence). Negative counts are rejected; the
    // write is idempotent for an unchanged value. Missing stage: MissingKey;
    // lease mismatch: Superseded.
    Failure setCreateStageMissingEvidence(const Lease &lease, const QString &attempt, int evidence);
    CreateStageResult readCreateStage(const Lease &lease, const QString &attempt);
    CreateStageListResult createStages(const Lease &lease);
    Failure clearCreateStage(const Lease &lease, const QString &attempt);
    // Issue #244 review 4576750798: durable in-flight marker for a key-rotation
    // announcement. The reissue action stages the fresh generation in the vault
    // and writes this record BEFORE the key_announced POST is dispatched; while
    // the record exists the (stream, key) row is pinned - deleting it could lose
    // the only copy of a generation the server may promote to current. The mark
    // clears only on a verified announcement echo or a definite refusal;
    // timeouts, 5xx and mere absence are not terminal.
    struct RotationStage {
        QString announcement; // the deterministic message uuid carried by the POST
        QString stream;
        QString key;          // the staged new generation
        bool dispatched = false; // set once the POST has left the process
        qint64 createdAt = 0;
    };
    struct RotationStageResult { Failure failure = Failure::Unavailable; std::optional<RotationStage> value; };
    struct RotationStageListResult { Failure failure = Failure::Unavailable; QVector<RotationStage> values; };
    Failure putRotationStage(const Lease &lease, const RotationStage &stage);
    RotationStageResult readRotationStage(const Lease &lease, const Address &address);
    // Enumerate the pinned stages; an empty stream lists the whole device.
    RotationStageListResult rotationStages(const Lease &lease, const QString &stream = {});
    Failure markRotationStageDispatched(const Lease &lease, const Address &address);
    Failure clearRotationStage(const Lease &lease, const Address &address);

    static constexpr qsizetype MaximumRecordBytes = 2048;
    static constexpr qint64 MaximumDatabaseBytes = 64 * 1024 * 1024;
    static constexpr qsizetype MaximumPendingRequests = 32;
    static constexpr qsizetype MaximumRotationStages = 32;
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
    QString rotationStagePrefix(const QString &stream) const;
    QString rotationStageAddress(const Address &address) const;
    std::optional<RotationStage> readRotationStageRecord(const Address &address, Failure &failure);
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
