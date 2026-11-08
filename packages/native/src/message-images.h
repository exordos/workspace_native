#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariantMap>
#include <memory>

class QQuickImageProvider;

namespace workspace {
class ApiClient;
class Session;
class FileClient;
struct FileResult;
struct MessageImageState;

// All manager API/lifetime operations belong to the GUI thread. The returned
// provider has separate shared state and may be called from a Qt loading thread.
class MessageImages final : public QObject {
    Q_OBJECT
    Q_PROPERTY(quint64 revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(quint64 scopeRevision READ scopeRevision NOTIFY scopeChanged)
public:
    static constexpr int MaximumEntries = 64;
    static constexpr int MaximumLeases = 4096;
    static constexpr int MaximumPipelineJobs = 2;
    static constexpr int MaximumPreviewWidth = 2048;
    static constexpr int MaximumPreviewHeight = 360;
    static constexpr qint64 MaximumRetainedBytes = 32 * 1024 * 1024;
    static constexpr qint64 MaximumSourcePixels = 16'000'000;
    struct Stats {
        int entries = 0, leases = 0, pipelineJobs = 0, queued = 0, decodeJobs = 0, previews = 0;
        qint64 previewBytes = 0;
    };
    explicit MessageImages(ApiClient *api, Session *session, FileClient *files, QObject *parent = nullptr);
    ~MessageImages() override;
    // 0 means invalid source, no ready owner, or bounded capacity exhausted.
    Q_INVOKABLE quint64 acquire(const QString &urn);
    Q_INVOKABLE void release(quint64 token);
    // Pure reads. Neither call starts/retries network or decoder work.
    Q_INVOKABLE QString imageUrl(const QString &urn) const;
    Q_INVOKABLE bool failed(const QString &urn) const;
    // Natural oriented dimensions, only after verified successful decode.
    Q_INVOKABLE QVariantMap info(const QString &urn) const;
    // Explicit action only; active/ready/unknown entries never start another job.
    Q_INVOKABLE bool retry(const QString &urn);
    quint64 revision() const { return m_revision; }
    quint64 scopeRevision() const { return m_scopeRevision; }
    Stats stats() const; // C++ resource witness only, not a QML debug model.
    // Caller transfers this provider to QQmlEngine. It never calls this manager.
    QQuickImageProvider *createImageProvider() const;
signals:
    void revisionChanged();
    void scopeChanged(); // Consumers discard old leases and acquire once again.
private:
    enum class Phase { Queued, Network, Decode, Ready, Failed, TransportFailed };
    struct Entry { Phase phase = Phase::Queued; int consumers = 0; quint64 order = 0, touched = 0, job = 0;
        QSize naturalSize; qint64 previewBytes = 0; };
    enum class JobPhase { Network, Decode };
    struct Job { QString scope, uuid; quint64 transfer = 0; JobPhase phase = JobPhase::Network; };
    QByteArray currentOwner() const;
    bool scopeCurrent() const;
    void refreshScope();
    void drain();
    void downloaded(const QString &scope, quint64 job, const FileResult &result);
    void decoded(const QString &scope, quint64 job, const QImage &image, const QSize &naturalSize);
    void notifyRevision();
    bool evictOne();
    bool reservePreview(qint64 bytes);
    void eraseImage(const QString &uuid);
    QString uuidFor(const QString &urn) const;
    QString cacheKey(const QString &uuid) const;
    QPointer<ApiClient> m_api;
    QPointer<Session> m_session;
    QPointer<FileClient> m_files;
    QByteArray m_owner;
    qint64 m_retainedBytes = 0;
    QString m_scope;
    quint64 m_generation = 0, m_revision = 0, m_scopeRevision = 0, m_nextLease = 0, m_nextJob = 0, m_order = 0;
    bool m_active = false, m_draining = false, m_stopping = false;
    QHash<QString, Entry> m_entries;
    QHash<quint64, QString> m_leases;
    QHash<quint64, Job> m_jobs; // <=2, including retired-scope decodes until they finish.
    std::shared_ptr<MessageImageState> m_images;
    QThread m_decodeThread;
    QPointer<QObject> m_decoder;
    Q_SIGNAL void decodeRequested(const QString &scope, quint64 job, const QByteArray &bytes);
};
} // namespace workspace
