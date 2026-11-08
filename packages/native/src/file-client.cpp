#include "file-client.h"
#include "api-client.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace workspace {
namespace {
const QRegularExpression UuidPattern(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
const QRegularExpression HashPattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
constexpr qint64 ChunkSize = 64 * 1024;
constexpr double MaxJsonInteger = 9007199254740991.0;

bool validUuid(const QString &uuid) { return UuidPattern.match(uuid).hasMatch(); }
int status(QNetworkReply *reply) { return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }
QString requestError(QNetworkReply *reply)
{
    const auto code = status(reply);
    if (code >= 300 && code < 400) return FileClient::tr("Redirects are not allowed.");
    if (code == 401) return FileClient::tr("Authentication failed (HTTP 401).");
    if (code == 403) return FileClient::tr("Access denied (HTTP 403).");
    if (code && (code < 200 || code >= 300)) return FileClient::tr("File request failed (HTTP %1).").arg(code);
    return FileClient::tr("File network request failed.");
}
QString safeName(QString name)
{
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f\\x7f]")), QStringLiteral("_"));
    name.replace(QRegularExpression(QStringLiteral("\\.{2,}")), QStringLiteral("."));
    return name.trimmed();
}

bool parseMetadata(const QByteArray &bytes, FileMetadata &metadata)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto object = document.object();
    if (!object.value("uuid").isString() || !validUuid(object.value("uuid").toString())
        || !object.value("name").isString() || object.value("name").toString().isEmpty()
        || !object.value("content_type").isString() || object.value("content_type").toString().isEmpty()
        || object.value("content_type").toString().size() > 255 || !object.value("size_bytes").isDouble())
        return false;
    const auto size = object.value("size_bytes").toDouble(-1);
    if (!std::isfinite(size) || size < 0 || size > MaxJsonInteger || std::floor(size) != size) return false;
    const auto stream = object.value("stream_uuid");
    if (!stream.isUndefined() && !stream.isNull() && (!stream.isString() || !validUuid(stream.toString()))) return false;
    const auto hash = object.value("hash");
    if (!hash.isUndefined() && !hash.isNull() && (!hash.isString() || !HashPattern.match(hash.toString()).hasMatch())) return false;
    metadata.uuid = object.value("uuid").toString().toLower();
    metadata.name = object.value("name").toString();
    metadata.contentType = object.value("content_type").toString();
    metadata.streamUuid = stream.toString().toLower();
    metadata.sizeBytes = static_cast<qint64>(size);
    metadata.sha256 = hash.toString().toLatin1().toLower();
    return true;
}

// A file can grow after selection. Never let multipart read beyond the size
// whose digest was computed, even when the underlying QFile now has more bytes.
class UploadFile final : public QFile {
public:
    UploadFile(const QString &path, qint64 size, QObject *parent) : QFile(path, parent), m_size(size) {}
    qint64 size() const override { return m_size; }
protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        return QFile::readData(data, std::min(maximum, std::max(qint64(0), m_size - pos())));
    }
private:
    qint64 m_size;
};
} // namespace

struct FileClient::Operation {
    Completion completion;
    quint64 generation = 0;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> timer;
    bool upload = false;
    bool memory = false;
    QByteArray bytes;
    QByteArray json;
    QString uuid;
    QString destination;
    FileMetadata metadata;
    std::unique_ptr<QSaveFile> output;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 received = 0;
};

FileKind FileMetadata::kind() const
{
    if (contentType.startsWith("image/", Qt::CaseInsensitive)) return FileKind::Image;
    if (contentType.startsWith("video/", Qt::CaseInsensitive)) return FileKind::Video;
    return FileKind::File;
}

QString FileMetadata::urn() const
{
    if (!validUuid(uuid)) return {};
    QStringList query;
    const auto append = [&query](const QString &key, const QString &value) {
        query.append(key + '=' + QString::fromLatin1(QUrl::toPercentEncoding(value)));
    };
    // URLSearchParams in the web renderer decodes '+' to space. QUrlQuery
    // deliberately preserves '+'; encode values explicitly for interoperability.
    if (!name.isEmpty()) append("name", name);
    if (!contentType.isEmpty()) append("content_type", contentType);
    append("size", QString::number(sizeBytes));
    const auto type = kind() == FileKind::Image ? "image" : kind() == FileKind::Video ? "video" : "file";
    return QStringLiteral("urn:%1:%2?%3").arg(QLatin1String(type), uuid.toLower(), query.join('&'));
}

FileClient::FileClient(ApiClient *api, QObject *parent, const FileTransferLimits &limits)
    : QObject(parent), m_api(api), m_limits(limits)
{
    m_limits.uploadBytes = std::clamp<qint64>(m_limits.uploadBytes, 1, 25 * 1024 * 1024);
    m_limits.metadataBytes = std::clamp<qsizetype>(m_limits.metadataBytes, 1, 1024 * 1024);
    m_limits.timeoutMs = std::clamp(m_limits.timeoutMs, 1, 300'000);
    if (api) {
        connect(api, &ApiClient::sessionChanged, this, [this] { cancelAll(); });
        connect(api, &QObject::destroyed, this, [this] { cancelAll(); });
    }
}

FileClient::~FileClient()
{
    const auto operations = m_operations;
    m_operations.clear();
    for (const auto &operation : operations) {
        if (operation->timer) { operation->timer->stop(); delete operation->timer; }
        if (operation->reply) {
            disconnect(operation->reply, nullptr, this, nullptr);
            operation->reply->abort();
            operation->reply->deleteLater();
        }
        if (operation->output) operation->output->cancelWriting();
    }
}

quint64 FileClient::begin(Completion completion)
{
    const auto id = ++m_nextOperation;
    auto operation = std::make_shared<Operation>();
    operation->completion = std::move(completion);
    operation->generation = m_api ? m_api->generation() : 0;
    operation->timer = new QTimer(this);
    operation->timer->setSingleShot(true);
    connect(operation->timer, &QTimer::timeout, this, [this, id] { fail(id, FileClient::tr("File transfer timed out.")); });
    m_operations.insert(id, operation);
    operation->timer->start(m_limits.timeoutMs);
    return id;
}

bool FileClient::current(quint64 id)
{
    const auto operation = m_operations.value(id);
    if (!operation) return false;
    if (!m_api || operation->generation != m_api->generation()) {
        fail(id, FileClient::tr("File transfer cancelled because the session changed."));
        return false;
    }
    return true;
}

void FileClient::complete(quint64 id, FileResult result)
{
    const auto operation = m_operations.take(id);
    if (!operation) return;
    if (operation->timer) { operation->timer->stop(); operation->timer->deleteLater(); }
    if (operation->reply) {
        disconnect(operation->reply, nullptr, this, nullptr);
        if (!operation->reply->isFinished()) operation->reply->abort();
        operation->reply->deleteLater();
    }
    if (operation->output && !result.ok()) {
        operation->output->cancelWriting();
        operation->output.reset();
    }
    const auto completion = std::move(operation->completion);
    if (completion) completion(result);
}

void FileClient::fail(quint64 id, const QString &error, int code)
{
    FileResult result;
    result.error = error;
    result.status = code;
    complete(id, result);
}

void FileClient::cancel(quint64 id) { fail(id, FileClient::tr("File transfer cancelled.")); }
void FileClient::cancelAll()
{
    const auto ids = m_operations.keys();
    QPointer<FileClient> guard(this);
    for (const auto id : ids) {
        cancel(id);
        if (!guard) return;
    }
}

quint64 FileClient::upload(const QString &path, const QString &streamUuid, const QString &name, Completion completion)
{
    const auto id = begin(std::move(completion));
    const QFileInfo info(path);
    if (!validUuid(streamUuid) || !info.isFile() || info.size() <= 0 || info.size() > m_limits.uploadBytes) {
        fail(id, FileClient::tr("Select a nonempty file of at most 25 MiB and a valid stream."));
        return id;
    }
    const auto uploadName = safeName(name.isEmpty() ? info.fileName() : name);
    if (uploadName.isEmpty()) { fail(id, FileClient::tr("Enter a valid file name.")); return id; }
    auto body = std::make_unique<QHttpMultiPart>(QHttpMultiPart::FormDataType);
    auto *file = new UploadFile(path, info.size(), body.get());
    if (!file->open(QIODevice::ReadOnly)) { fail(id, FileClient::tr("Cannot read the selected file.")); return id; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 hashed = 0;
    while (hashed < info.size()) {
        const auto bytes = file->read(std::min(ChunkSize, info.size() - hashed));
        if (bytes.isEmpty()) { fail(id, FileClient::tr("Cannot read the complete selected file.")); return id; }
        hash.addData(bytes);
        hashed += bytes.size();
    }
    if (!file->seek(0)) { fail(id, FileClient::tr("Cannot rewind the selected file.")); return id; }
    const auto operation = m_operations.value(id);
    operation->upload = true;
    operation->metadata.streamUuid = streamUuid.toLower();
    operation->metadata.name = uploadName;
    operation->metadata.contentType = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchContent).name();
    operation->metadata.sizeBytes = info.size();
    operation->metadata.sha256 = hash.result().toHex();
    auto headerName = uploadName.toLatin1();
    for (auto &character : headerName)
        if (static_cast<unsigned char>(character) < 0x20 || static_cast<unsigned char>(character) > 0x7e || character == '"' || character == '\\') character = '_';
    QHttpPart binary;
    binary.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"file\"; filename=\"" + headerName + '"');
    binary.setHeader(QNetworkRequest::ContentTypeHeader, operation->metadata.contentType);
    binary.setBodyDevice(file);
    body->append(binary);
    const auto appendText = [&body](const QByteArray &key, const QByteArray &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"" + key + '"');
        part.setBody(value);
        body->append(part);
    };
    appendText("stream_uuid", streamUuid.toLower().toUtf8());
    appendText("name", uploadName.toUtf8());
    operation->reply = m_api ? m_api->startFileUpload(body.get()) : nullptr;
    if (!operation->reply) { fail(id, FileClient::tr("Sign in to upload files.")); return id; }
    body.release(); // ApiClient transferred the multipart lifetime to its reply.
    operation->reply->setReadBufferSize(m_limits.metadataBytes + 1);
    connect(operation->reply, &QNetworkReply::readyRead, this, [this, id] { readMetadata(id); });
    connect(operation->reply, &QNetworkReply::finished, this, [this, id] { finishMetadata(id); });
    return id;
}

quint64 FileClient::download(const QString &uuid, const QString &destination, Completion completion)
{
    const auto id = begin(std::move(completion));
    if (!validUuid(uuid) || destination.isEmpty()) { fail(id, FileClient::tr("Invalid file identifier or destination.")); return id; }
    const auto operation = m_operations.value(id);
    operation->uuid = uuid.toLower();
    operation->destination = destination;
    operation->reply = m_api ? m_api->startFileMetadata(operation->uuid) : nullptr;
    if (!operation->reply) { fail(id, FileClient::tr("Sign in to download files.")); return id; }
    operation->reply->setReadBufferSize(m_limits.metadataBytes + 1);
    connect(operation->reply, &QNetworkReply::readyRead, this, [this, id] { readMetadata(id); });
    connect(operation->reply, &QNetworkReply::finished, this, [this, id] { finishMetadata(id); });
    return id;
}

quint64 FileClient::downloadBytes(const QString &uuid, Completion completion)
{
    const auto id = begin(std::move(completion));
    if (!validUuid(uuid)) { fail(id, FileClient::tr("Invalid file identifier.")); return id; }
    const auto operation = m_operations.value(id);
    operation->memory = true;
    operation->uuid = uuid.toLower();
    operation->reply = m_api ? m_api->startFileMetadata(operation->uuid) : nullptr;
    if (!operation->reply) { fail(id, FileClient::tr("Sign in to download files.")); return id; }
    operation->reply->setReadBufferSize(m_limits.metadataBytes + 1);
    connect(operation->reply, &QNetworkReply::readyRead, this, [this, id] { readMetadata(id); });
    connect(operation->reply, &QNetworkReply::finished, this, [this, id] { finishMetadata(id); });
    return id;
}

void FileClient::readMetadata(quint64 id)
{
    if (!current(id)) return;
    const auto operation = m_operations.value(id);
    const auto code = status(operation->reply);
    if (code && (code < 200 || code >= 300)) { fail(id, requestError(operation->reply), code); return; }
    operation->json += operation->reply->read(m_limits.metadataBytes - operation->json.size() + 1);
    if (operation->json.size() > m_limits.metadataBytes)
        fail(id, FileClient::tr("File metadata exceeds the allowed size."), code);
}

void FileClient::finishMetadata(quint64 id)
{
    if (!current(id)) return;
    const QPointer<FileClient> guard(this);
    readMetadata(id);
    if (!guard) return;
    const auto operation = m_operations.value(id);
    if (!operation) return;
    auto *reply = operation->reply.data();
    const auto code = status(reply);
    if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300) { fail(id, requestError(reply), code); return; }
    FileMetadata metadata;
    if (!parseMetadata(operation->json, metadata)) { fail(id, FileClient::tr("Server returned invalid file metadata."), code); return; }
    if (operation->upload) {
        const auto &expected = operation->metadata;
        if (metadata.streamUuid != expected.streamUuid || metadata.sizeBytes != expected.sizeBytes
            || metadata.sha256 != expected.sha256 || metadata.name != expected.name
            || metadata.contentType != expected.contentType) {
            fail(id, FileClient::tr("Uploaded file metadata does not match the selected file."), code);
            return;
        }
        FileResult result;
        result.metadata = metadata;
        result.status = code;
        result.checksumVerified = true;
        complete(id, result);
        return;
    }
    if (metadata.uuid != operation->uuid) { fail(id, FileClient::tr("Server returned a different file identifier."), code); return; }
    if (operation->memory && metadata.sizeBytes > MemoryDownloadLimitBytes) {
        fail(id, FileClient::tr("File exceeds the allowed in-memory size of 25 MiB."), code); return;
    }
    operation->metadata = metadata;
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    operation->reply.clear();
    operation->json.clear();
    startDownload(id);
}

void FileClient::startDownload(quint64 id)
{
    if (!current(id)) return;
    const auto operation = m_operations.value(id);
    if (!operation->memory) {
        const QStorageInfo storage(QFileInfo(operation->destination).absolutePath());
        if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0
            && operation->metadata.sizeBytes > storage.bytesAvailable()) {
            fail(id, FileClient::tr("Not enough free space to save the file.")); return;
        }
        operation->output = std::make_unique<QSaveFile>(operation->destination);
        operation->output->setDirectWriteFallback(false);
        if (!operation->output->open(QIODevice::WriteOnly)) { fail(id, FileClient::tr("Cannot create the destination file.")); return; }
    }
    operation->reply = m_api->startFileDownload(operation->uuid);
    if (!operation->reply) { fail(id, FileClient::tr("Sign in to download files.")); return; }
    operation->reply->setReadBufferSize(ChunkSize);
    connect(operation->reply, &QNetworkReply::readyRead, this, [this, id] { readDownload(id); });
    connect(operation->reply, &QNetworkReply::finished, this, [this, id] { finishDownload(id); });
}

void FileClient::readDownload(quint64 id)
{
    if (!current(id)) return;
    const auto operation = m_operations.value(id);
    auto *reply = operation->reply.data();
    const auto code = status(reply);
    if (code && code != 200) { fail(id, requestError(reply), code); return; }
    const auto contentLength = reply->header(QNetworkRequest::ContentLengthHeader);
    if (contentLength.isValid() && contentLength.toLongLong() != operation->metadata.sizeBytes) {
        fail(id, FileClient::tr("Download size does not match file metadata."), code); return;
    }
    while (reply->bytesAvailable() > 0) {
        const auto bytes = reply->read(std::min(ChunkSize, operation->metadata.sizeBytes - operation->received + 1));
        if (bytes.isEmpty()) break;
        if (bytes.size() > operation->metadata.sizeBytes - operation->received) {
            fail(id, FileClient::tr("Download exceeds the declared file size."), code); return;
        }
        // Metadata for memory mode was capped before the binary GET; the
        // shared declared-size check above bounds every append to that cap.
        if (operation->memory) operation->bytes.append(bytes);
        else if (operation->output->write(bytes) != bytes.size()) { fail(id, FileClient::tr("Cannot write the destination file."), code); return; }
        operation->hash.addData(bytes);
        operation->received += bytes.size();
    }
}

void FileClient::finishDownload(quint64 id)
{
    if (!current(id)) return;
    const QPointer<FileClient> guard(this);
    readDownload(id);
    if (!guard) return;
    if (!current(id)) return;
    const auto operation = m_operations.value(id);
    auto *reply = operation->reply.data();
    const auto code = status(reply);
    if (reply->error() != QNetworkReply::NoError || code != 200) { fail(id, requestError(reply), code); return; }
    if (operation->received != operation->metadata.sizeBytes) { fail(id, FileClient::tr("Downloaded file is incomplete."), code); return; }
    if (!operation->metadata.sha256.isEmpty() && operation->hash.result().toHex() != operation->metadata.sha256) {
        fail(id, FileClient::tr("Downloaded file checksum does not match."), code); return;
    }
    // Check ownership immediately before the only operation that replaces a file.
    if (!current(id)) return;
    if (!operation->memory && !operation->output->commit()) { fail(id, FileClient::tr("Cannot commit the destination file."), code); return; }
    FileResult result;
    result.metadata = operation->metadata;
    result.path = operation->destination;
    if (operation->memory) result.bytes = std::move(operation->bytes);
    result.status = code;
    result.checksumVerified = !operation->metadata.sha256.isEmpty();
    complete(id, result);
}
} // namespace workspace
