#include "message-images.h"
#include "api-client.h"
#include "workspace-reference.h"
#include "file-client.h"
#include "native-image-input.h"
#include "session.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <QQuickImageProvider>
#include <QUuid>
#include <limits>

namespace workspace {
struct MessageImageState {
    MessageImageState() : transparent([] { QImage image(1, 1, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); return image; }()) {}
    QMutex mutex;
    QHash<QString, QImage> images;
    const QImage transparent;
};
namespace {
// Reads only already-published bounded previews. No manager, network, decode,
// scaling, event loop, condition variable or wait can be reached from here.
class CachedMessageProvider final : public QQuickImageProvider {
public:
    explicit CachedMessageProvider(std::shared_ptr<MessageImageState> images)
        : QQuickImageProvider(QQuickImageProvider::Image), m_images(std::move(images)) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &) override
    {
        QImage image;
        { QMutexLocker lock(&m_images->mutex); image = m_images->images.value(id);
            if (image.isNull()) image = m_images->transparent; }
        // A retired/invalid opaque URL can race with logout in Qt's loader.
        // Manager imageUrl()/failed() are the authority, not provider success.
        if (size) *size = image.size();
        return image;
    }
private:
    std::shared_ptr<MessageImageState> m_images;
};

struct DecodedPreview { QImage image; QSize naturalSize; };
DecodedPreview preview(const QByteArray &bytes)
{
    QBuffer buffer; buffer.setData(nativeImageReaderInput(bytes));
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer); reader.setAutoTransform(true);
    const auto format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "gif" && format != "webp") return {};
    const auto size = reader.size();
    if (size.width() <= 0 || size.height() <= 0
        || qint64(size.width()) * size.height() > MessageImages::MaximumSourcePixels) return {};
    // One first-frame decode on the serial worker. Never retain encoded bytes
    // or the original decoded canvas in the manager/provider cache.
    const auto source = reader.read();
    if (source.isNull() || source.width() <= 0 || source.height() <= 0
        || qint64(source.width()) * source.height() > MessageImages::MaximumSourcePixels) return {};
    const qreal scale = std::min({qreal(1), qreal(MessageImages::MaximumPreviewWidth) / source.width(),
                                 qreal(MessageImages::MaximumPreviewHeight) / source.height()});
    // One shared scale, rounded down, with a minimum representable pixel on
    // extreme aspect ratios. Entire canvas, no crop, circle or upscaling.
    const QSize target(qMax(1, int(std::floor(source.width() * scale))),
                       qMax(1, int(std::floor(source.height() * scale))));
    // Materialize pixels only: a scaled/copy-converted QImage can otherwise
    // retain uncounted source text/profile metadata. No original is cached.
    QImage image(target, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return {};
    image.fill(Qt::transparent);
    { QPainter paint(&image); paint.setRenderHint(QPainter::SmoothPixmapTransform);
        paint.drawImage(QRectF(QPointF{}, target), source); }
    return {image, source.size()};
}
} // namespace

class MessageDecoder final : public QObject {
    Q_OBJECT
public slots:
    void decode(const QString &scope, quint64 job, const QByteArray &bytes)
    {
        auto result = QThread::currentThread()->isInterruptionRequested() ? DecodedPreview{} : preview(bytes);
        // Notify on a later worker turn: the queued decode argument and original
        // encoded bytes/full decode are gone before GUI releases this job slot.
        QMetaObject::invokeMethod(this, [this, scope, job, result = std::move(result)] {
            emit completed(scope, job, result.image, result.naturalSize);
        }, Qt::QueuedConnection);
    }
signals:
    void completed(const QString &scope, quint64 job, const QImage &image, const QSize &naturalSize);
};

MessageImages::MessageImages(ApiClient *api, Session *session, FileClient *files, QObject *parent)
    : QObject(parent), m_api(api), m_session(session), m_files(files),
      m_images(std::make_shared<MessageImageState>()), m_decodeThread(this)
{
    Q_ASSERT(QCoreApplication::instance() && thread() == QCoreApplication::instance()->thread());
    auto *decoder = new MessageDecoder;
    m_decoder = decoder; decoder->moveToThread(&m_decodeThread);
    m_decodeThread.setObjectName("messageImageSerialDecoder");
    connect(&m_decodeThread, &QThread::finished, decoder, &QObject::deleteLater);
    connect(this, &MessageImages::decodeRequested, decoder, &MessageDecoder::decode, Qt::QueuedConnection);
    connect(decoder, &MessageDecoder::completed, this, &MessageImages::decoded, Qt::QueuedConnection);
    m_decodeThread.start();
    if (api) { connect(api, &ApiClient::sessionChanged, this, &MessageImages::refreshScope);
        connect(api, &QObject::destroyed, this, &MessageImages::refreshScope); }
    if (session) { connect(session, &Session::authenticated, this, &MessageImages::refreshScope);
        connect(session, &Session::signedOut, this, &MessageImages::refreshScope);
        connect(session, &Session::busyChanged, this, &MessageImages::refreshScope);
        connect(session, &QObject::destroyed, this, &MessageImages::refreshScope); }
    if (files) connect(files, &QObject::destroyed, this, &MessageImages::refreshScope);
    refreshScope();
}

MessageImages::~MessageImages()
{
    m_stopping = true;
    if (m_decoder) disconnect(m_decoder, nullptr, this, nullptr);
    const auto jobs = m_jobs; m_jobs.clear(); m_entries.clear(); m_leases.clear();
    { QMutexLocker lock(&m_images->mutex); m_images->images.clear(); }
    for (const auto &job : jobs)
        if (job.phase == JobPhase::Network && job.transfer && m_files) m_files->cancel(job.transfer);
    m_decodeThread.requestInterruption(); m_decodeThread.quit(); m_decodeThread.wait();
}

QString MessageImages::uuidFor(const QString &urn) const
{
    const auto reference = parseWorkspaceReference(urn);
    return reference && reference->kind == WorkspaceReferenceKind::Image ? reference->id : QString{};
}
QString MessageImages::cacheKey(const QString &uuid) const { return m_scope + '/' + uuid; }
void MessageImages::eraseImage(const QString &uuid)
{
    QMutexLocker lock(&m_images->mutex); m_images->images.remove(cacheKey(uuid));
    const auto entry = m_entries.find(uuid);
    if (entry != m_entries.end()) { m_retainedBytes -= entry->previewBytes; entry->previewBytes = 0; }
}
void MessageImages::notifyRevision() { ++m_revision; emit revisionChanged(); }

QByteArray MessageImages::currentOwner() const
{
    return m_api && m_session && m_files && m_session->ready()
        ? QJsonDocument(QJsonArray{m_api->server().toString(QUrl::FullyEncoded),
            m_session->project(), m_session->user()}).toJson(QJsonDocument::Compact) : QByteArray{};
}
bool MessageImages::scopeCurrent() const
{
    return m_active && m_api && m_api->generation() == m_generation && currentOwner() == m_owner;
}
void MessageImages::refreshScope()
{
    if (m_stopping) return;
    const auto owner = currentOwner();
    const auto generation = m_api ? m_api->generation() : 0;
    if (owner == m_owner && generation == m_generation) return;
    m_owner = owner; m_generation = generation; m_active = !owner.isEmpty();
    m_scope = QUuid::createUuid().toString(QUuid::WithoutBraces); ++m_scopeRevision;
    const auto scope = m_scope;
    m_entries.clear(); m_leases.clear(); m_retainedBytes = 0;
    { QMutexLocker lock(&m_images->mutex); m_images->images.clear(); }
    // Retired decode tickets keep their payload slots until the worker completes.
    // Only our exact transfers are cancelled; FileClient may have other consumers.
    const auto jobs = m_jobs;
    const QPointer<MessageImages> guard(this);
    for (auto it = jobs.cbegin(); it != jobs.cend(); ++it) if (it->phase == JobPhase::Network) {
        m_jobs.remove(it.key());
        if (it->transfer && m_files) m_files->cancel(it->transfer);
        if (!guard || scope != m_scope) return;
    }
    notifyRevision(); if (!guard || scope != m_scope) return;
    emit scopeChanged(); if (!guard || scope != m_scope) return;
    drain();
}

bool MessageImages::evictOne()
{
    auto oldest = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        if (!it->consumers && !it->job && (oldest == m_entries.end() || it->touched < oldest->touched)) oldest = it;
    if (oldest == m_entries.end()) return false;
    eraseImage(oldest.key()); m_entries.erase(oldest); return true;
}
quint64 MessageImages::acquire(const QString &urn)
{
    const QPointer<MessageImages> ownerGuard(this);
    if (!scopeCurrent()) { refreshScope(); if (!ownerGuard) return 0; }
    const auto uuid = uuidFor(urn);
    constexpr quint64 MaximumSafeJsToken = (quint64(1) << 53) - 1;
    if (m_stopping || !m_active || uuid.isEmpty() || m_leases.size() >= MaximumLeases
        || m_nextLease == MaximumSafeJsToken) return 0;
    if (!m_entries.contains(uuid)) {
        if (m_entries.size() >= MaximumEntries && !evictOne()) return 0;
        Entry entry; entry.order = entry.touched = ++m_order; m_entries.insert(uuid, entry);
    }
    const auto token = ++m_nextLease; const auto scope = m_scope;
    auto &entry = m_entries[uuid]; ++entry.consumers; entry.touched = ++m_order;
    const bool retry = entry.phase == Phase::Failed || entry.phase == Phase::TransportFailed;
    if (retry) { entry.phase = Phase::Queued; entry.order = entry.touched; }
    m_leases.insert(token, uuid);
    const QPointer<MessageImages> guard(this);
    if (retry) {
        notifyRevision();
        if (!guard || scope != m_scope) return 0;
    }
    drain(); if (!guard || scope != m_scope || !m_leases.contains(token)) return 0;
    return token;
}
bool MessageImages::reservePreview(qint64 bytes)
{
    if (bytes <= 0 || bytes > MaximumRetainedBytes) return false;
    while (m_retainedBytes > MaximumRetainedBytes - bytes)
        if (!evictOne()) return false;
    return true;
}
bool MessageImages::retry(const QString &urn)
{
    const QPointer<MessageImages> guard(this);
    if (!scopeCurrent()) { refreshScope(); if (!guard) return false; }
    const auto uuid = uuidFor(urn); auto entry = m_entries.find(uuid);
    if (m_stopping || !scopeCurrent() || uuid.isEmpty() || entry == m_entries.end() || !entry->consumers
        || (entry->phase != Phase::Failed && entry->phase != Phase::TransportFailed)) return false;
    const auto scope = m_scope;
    entry->phase = Phase::Queued; entry->order = entry->touched = ++m_order;
    notifyRevision();
    if (!guard || scope != m_scope) return false;
    drain();
    return guard && scope == m_scope && m_entries.contains(uuid);
}
void MessageImages::release(quint64 token)
{
    const auto lease = m_leases.find(token); if (lease == m_leases.end()) return;
    const auto uuid = lease.value(); m_leases.erase(lease);
    auto entry = m_entries.find(uuid); if (entry == m_entries.end() || --entry->consumers) return;
    entry->touched = ++m_order;
    if (entry->phase == Phase::Ready || entry->phase == Phase::Failed) return; // bounded retained hit/failure
    const auto job = m_jobs.value(entry->job); const auto ticket = entry->job;
    m_entries.erase(entry);
    const QPointer<MessageImages> guard(this);
    if (ticket && job.phase == JobPhase::Network) {
        m_jobs.remove(ticket);
        if (job.transfer && m_files) m_files->cancel(job.transfer);
        if (!guard) return;
    } // A queued/active decode is bounded but not forcibly interrupted; its result is now unowned.
    drain();
}
void MessageImages::drain()
{
    if (m_draining || m_stopping || !scopeCurrent() || !m_files) return;
    m_draining = true;
    const QPointer<MessageImages> guard(this);
    while (scopeCurrent() && m_files && m_jobs.size() < MaximumPipelineJobs) {
        auto next = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (it->phase == Phase::Queued && it->consumers && (next == m_entries.end() || it->order < next->order)) next = it;
        if (next == m_entries.end()) break;
        if (m_nextJob == std::numeric_limits<quint64>::max()) { next->phase = Phase::Failed; continue; }
        const auto job = ++m_nextJob; const auto scope = m_scope, uuid = next.key();
        next->phase = Phase::Network; next->job = job;
        m_jobs.insert(job, Job{scope, uuid, 0, JobPhase::Network});
        const auto transfer = m_files->downloadBytes(uuid, [guard, scope, job](const FileResult &result) {
            if (guard) guard->downloaded(scope, job, result);
        });
        if (!guard) return;
        auto active = m_jobs.find(job);
        if (active != m_jobs.end() && active->phase == JobPhase::Network) active->transfer = transfer;
    }
    m_draining = false;
}
void MessageImages::downloaded(const QString &scope, quint64 job, const FileResult &result)
{
    if (m_stopping) return;
    // FileClient may cancel synchronously in an earlier Api sessionChanged slot.
    // Do not publish old-owner failure before our own scopeChanged slot runs.
    if (!scopeCurrent()) { refreshScope(); return; }
    auto ticket = m_jobs.find(job); if (ticket == m_jobs.end() || ticket->phase != JobPhase::Network) return;
    const auto uuid = ticket->uuid;
    auto entry = m_entries.find(uuid);
    if (scope != m_scope || entry == m_entries.end() || entry->job != job || !entry->consumers) { m_jobs.erase(ticket); drain(); return; }
    if (!result.ok()) {
        m_jobs.erase(ticket); entry->job = 0; entry->phase = Phase::TransportFailed;
        const QPointer<MessageImages> guard(this); notifyRevision();
        if (guard) drain();
        return;
    }
    ticket->phase = JobPhase::Decode; ticket->transfer = 0; entry->phase = Phase::Decode;
    emit decodeRequested(scope, job, result.bytes);
    notifyRevision();
}
void MessageImages::decoded(const QString &scope, quint64 job, const QImage &image, const QSize &naturalSize)
{
    if (m_stopping) return;
    auto ticket = m_jobs.find(job); if (ticket == m_jobs.end() || ticket->phase != JobPhase::Decode) return;
    const auto uuid = ticket->uuid; m_jobs.erase(ticket);
    if (!scopeCurrent()) {
        const QPointer<MessageImages> guard(this); refreshScope();
        if (guard) drain();
        return;
    }
    auto entry = m_entries.find(uuid);
    if (scope == m_scope && entry != m_entries.end() && entry->job == job && entry->consumers) {
        const auto bytes = qint64(image.sizeInBytes());
        const bool retain = !image.isNull() && !naturalSize.isEmpty() && reservePreview(bytes);
        entry = m_entries.find(uuid); // LRU removal above never owns this leased row.
        entry->job = 0; entry->phase = retain ? Phase::Ready : Phase::Failed;
        if (retain) {
            entry->naturalSize = naturalSize; entry->previewBytes = bytes; m_retainedBytes += bytes;
            QMutexLocker lock(&m_images->mutex); m_images->images.insert(cacheKey(uuid), image);
        }
        const QPointer<MessageImages> guard(this); notifyRevision();
        if (!guard) return;
    }
    drain();
}
QString MessageImages::imageUrl(const QString &urn) const
{
    const auto uuid = uuidFor(urn); const auto entry = m_entries.constFind(uuid);
    return scopeCurrent() && !uuid.isEmpty() && entry != m_entries.cend() && entry->phase == Phase::Ready
        ? "image://messageimages/" + cacheKey(uuid) : QString{};
}
bool MessageImages::failed(const QString &urn) const
{
    const auto entry = m_entries.constFind(uuidFor(urn));
    return scopeCurrent() && entry != m_entries.cend()
        && (entry->phase == Phase::Failed || entry->phase == Phase::TransportFailed);
}
QVariantMap MessageImages::info(const QString &urn) const
{
    const auto entry = m_entries.constFind(uuidFor(urn));
    return scopeCurrent() && entry != m_entries.cend() && entry->phase == Phase::Ready
        ? QVariantMap{{"width", entry->naturalSize.width()}, {"height", entry->naturalSize.height()}} : QVariantMap{};
}
MessageImages::Stats MessageImages::stats() const
{
    Stats result; result.entries = m_entries.size(); result.leases = m_leases.size(); result.pipelineJobs = m_jobs.size();
    for (const auto &entry : m_entries) if (entry.phase == Phase::Queued) ++result.queued;
    for (const auto &job : m_jobs) if (job.phase == JobPhase::Decode) ++result.decodeJobs;
    QMutexLocker lock(&m_images->mutex); result.previews = m_images->images.size();
    for (const auto &image : m_images->images) result.previewBytes += image.sizeInBytes();
    return result;
}
QQuickImageProvider *MessageImages::createImageProvider() const { return new CachedMessageProvider(m_images); }
} // namespace workspace
#include "message-images.moc"
