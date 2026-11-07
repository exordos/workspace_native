#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

namespace workspace {

class ApiClient;

enum class FileKind { File, Image, Video };

struct FileMetadata {
    QString uuid;
    QString streamUuid;
    QString name;
    QString contentType;
    qint64 sizeBytes = 0;
    QByteArray sha256; // Empty when the server supplies no digest.

    FileKind kind() const;
    QString urn() const;
};

struct FileResult {
    FileMetadata metadata;
    QString path;
    QString error;
    int status = 0;
    bool checksumVerified = false;

    bool ok() const { return status >= 200 && status < 300 && error.isEmpty(); }
};

struct FileTransferLimits {
    // Matches the existing desktop composer's upload policy, not a backend limit.
    qint64 uploadBytes = 25 * 1024 * 1024;
    qsizetype metadataBytes = 1024 * 1024;
    int timeoutMs = 300'000;
};

class FileClient final : public QObject {
    Q_OBJECT

public:
    using Completion = std::function<void(const FileResult &)>;
    explicit FileClient(ApiClient *api, QObject *parent = nullptr,
                        const FileTransferLimits &limits = {});
    ~FileClient() override;

    // Local validation can complete synchronously. Cancellation completes once
    // with status 0; destruction suppresses callbacks and removes staged files.
    quint64 upload(const QString &path, const QString &streamUuid, const QString &name,
                   Completion completion);
    quint64 download(const QString &uuid, const QString &destination, Completion completion);
    void cancel(quint64 operation);
    void cancelAll();

private:
    struct Operation;
    quint64 begin(Completion completion);
    void fail(quint64 operation, const QString &error, int status = 0);
    void complete(quint64 operation, FileResult result);
    void readMetadata(quint64 operation);
    void finishMetadata(quint64 operation);
    void startDownload(quint64 operation);
    void readDownload(quint64 operation);
    void finishDownload(quint64 operation);
    bool current(quint64 operation);

    QPointer<ApiClient> m_api;
    FileTransferLimits m_limits;
    quint64 m_nextOperation = 0;
    QHash<quint64, std::shared_ptr<Operation>> m_operations;
};

} // namespace workspace
