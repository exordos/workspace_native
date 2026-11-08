#include "avatar-images.h"
#include "api-client.h"
#include "domain.h"
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
#include <QQuickImageProvider>
#include <QUuid>
#include <limits>

namespace workspace {
struct AvatarImageState {
    AvatarImageState() : transparent([] { QImage image(1, 1, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); return image; }()) {}
    QMutex mutex;
    QHash<QString, QImage> images;
    const QImage transparent;
};
namespace {
// Reads only already-published fixed-size images. No manager, network, decode,
// scaling, event loop, condition variable or wait can be reached from here.
class CachedAvatarProvider final : public QQuickImageProvider {
public:
    explicit CachedAvatarProvider(std::shared_ptr<AvatarImageState> images)
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
    std::shared_ptr<AvatarImageState> m_images;
};

QImage thumbnail(const QByteArray &bytes)
{
    QBuffer buffer; buffer.setData(nativeImageReaderInput(bytes));
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer); reader.setAutoTransform(true);
    const auto format = reader.format().toLower();
    // SVG is rejected before size()/read(); no renderer or external resource.
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "gif" && format != "webp") return {};
    const auto size = reader.size();
    if (size.width() <= 0 || size.height() <= 0
        || qint64(size.width()) * size.height() > AvatarImages::MaximumSourcePixels) return {};
    // Decode one bounded canvas. Extreme aspect ratios must never request an
    // expanded square-fit decode larger than the source or overflowing int.
    const auto source = reader.read(); // First static frame only; never jump/animate.
    if (source.isNull() || qint64(source.width()) * source.height() > AvatarImages::MaximumSourcePixels) return {};
    const qreal side = qMin(source.width(), source.height());
    const QRectF crop((source.width() - side) / 2, (source.height() - side) / 2, side, side);
    const QSize target(AvatarImages::ThumbnailEdge, AvatarImages::ThumbnailEdge);
    QImage result(target, QImage::Format_ARGB32_Premultiplied); result.fill(Qt::transparent);
    QImage mask(target, QImage::Format_ARGB32_Premultiplied); mask.fill(Qt::transparent);
    {
        QPainter paint(&mask); paint.setRenderHint(QPainter::Antialiasing);
        paint.setPen(Qt::NoPen); paint.setBrush(Qt::white); paint.drawEllipse(QRectF(QPointF{}, target));
    }
    {
        QPainter paint(&result); paint.setRenderHint(QPainter::SmoothPixmapTransform);
        paint.drawImage(QRectF(QPointF{}, target), source, crop);
        paint.setCompositionMode(QPainter::CompositionMode_DestinationIn); paint.drawImage(0, 0, mask);
    }
    return result;
}
} // namespace

class AvatarDecoder final : public QObject {
    Q_OBJECT
public slots:
    void decode(const QString &scope, quint64 job, const QByteArray &bytes)
    {
        auto image = QThread::currentThread()->isInterruptionRequested() ? QImage{} : thumbnail(bytes);
        // Notify on a later worker turn: the queued decode argument and original
        // encoded bytes/full decode are gone before GUI releases this job slot.
        QMetaObject::invokeMethod(this, [this, scope, job, image = std::move(image)] {
            emit completed(scope, job, image);
        }, Qt::QueuedConnection);
    }
signals:
    void completed(const QString &scope, quint64 job, const QImage &image);
};

AvatarImages::AvatarImages(ApiClient *api, Session *session, FileClient *files, QObject *parent)
    : QObject(parent), m_api(api), m_session(session), m_files(files),
      m_images(std::make_shared<AvatarImageState>()), m_decodeThread(this)
{
    Q_ASSERT(QCoreApplication::instance() && thread() == QCoreApplication::instance()->thread());
    auto *decoder = new AvatarDecoder;
    m_decoder = decoder; decoder->moveToThread(&m_decodeThread);
    m_decodeThread.setObjectName("avatarSerialDecoder");
    connect(&m_decodeThread, &QThread::finished, decoder, &QObject::deleteLater);
    connect(this, &AvatarImages::decodeRequested, decoder, &AvatarDecoder::decode, Qt::QueuedConnection);
    connect(decoder, &AvatarDecoder::completed, this, &AvatarImages::decoded, Qt::QueuedConnection);
    m_decodeThread.start();
    if (api) { connect(api, &ApiClient::sessionChanged, this, &AvatarImages::refreshScope);
        connect(api, &QObject::destroyed, this, &AvatarImages::refreshScope); }
    if (session) { connect(session, &Session::authenticated, this, &AvatarImages::refreshScope);
        connect(session, &Session::signedOut, this, &AvatarImages::refreshScope);
        connect(session, &Session::busyChanged, this, &AvatarImages::refreshScope);
        connect(session, &QObject::destroyed, this, &AvatarImages::refreshScope); }
    if (files) connect(files, &QObject::destroyed, this, &AvatarImages::refreshScope);
    refreshScope();
}

AvatarImages::~AvatarImages()
{
    m_stopping = true;
    if (m_decoder) disconnect(m_decoder, nullptr, this, nullptr);
    const auto jobs = m_jobs; m_jobs.clear(); m_entries.clear(); m_leases.clear();
    { QMutexLocker lock(&m_images->mutex); m_images->images.clear(); }
    for (const auto &job : jobs)
        if (job.phase == JobPhase::Network && job.transfer && m_files) m_files->cancel(job.transfer);
    m_decodeThread.requestInterruption(); m_decodeThread.quit(); m_decodeThread.wait();
}

QString AvatarImages::uuidFor(const QString &urn) const
{
    return imageAvatarReference(urn).mid(10).toLower();
}
QString AvatarImages::cacheKey(const QString &uuid) const { return m_scope + '/' + uuid; }
void AvatarImages::eraseImage(const QString &uuid)
{
    QMutexLocker lock(&m_images->mutex); m_images->images.remove(cacheKey(uuid));
}
void AvatarImages::notifyRevision() { ++m_revision; emit revisionChanged(); }

QByteArray AvatarImages::currentOwner() const
{
    return m_api && m_session && m_files && m_session->ready()
        ? QJsonDocument(QJsonArray{m_api->server().toString(QUrl::FullyEncoded),
            m_session->project(), m_session->user()}).toJson(QJsonDocument::Compact) : QByteArray{};
}
bool AvatarImages::scopeCurrent() const
{
    return m_active && m_api && m_api->generation() == m_generation && currentOwner() == m_owner;
}
void AvatarImages::refreshScope()
{
    if (m_stopping) return;
    const auto owner = currentOwner();
    const auto generation = m_api ? m_api->generation() : 0;
    if (owner == m_owner && generation == m_generation) return;
    m_owner = owner; m_generation = generation; m_active = !owner.isEmpty();
    m_scope = QUuid::createUuid().toString(QUuid::WithoutBraces); ++m_scopeRevision;
    const auto scope = m_scope;
    m_entries.clear(); m_leases.clear();
    { QMutexLocker lock(&m_images->mutex); m_images->images.clear(); }
    // Retired decode tickets keep their payload slots until the worker completes.
    // Only our exact transfers are cancelled; FileClient may have other consumers.
    const auto jobs = m_jobs;
    const QPointer<AvatarImages> guard(this);
    for (auto it = jobs.cbegin(); it != jobs.cend(); ++it) if (it->phase == JobPhase::Network) {
        m_jobs.remove(it.key());
        if (it->transfer && m_files) m_files->cancel(it->transfer);
        if (!guard || scope != m_scope) return;
    }
    notifyRevision(); if (!guard || scope != m_scope) return;
    emit scopeChanged(); if (!guard || scope != m_scope) return;
    drain();
}

bool AvatarImages::evictOne()
{
    auto oldest = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        if (!it->consumers && !it->job && (oldest == m_entries.end() || it->touched < oldest->touched)) oldest = it;
    if (oldest == m_entries.end()) return false;
    eraseImage(oldest.key()); m_entries.erase(oldest); return true;
}
quint64 AvatarImages::acquire(const QString &urn)
{
    const QPointer<AvatarImages> ownerGuard(this);
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
    m_leases.insert(token, uuid);
    const QPointer<AvatarImages> guard(this);
    drain(); if (!guard || scope != m_scope || !m_leases.contains(token)) return 0;
    return token;
}
void AvatarImages::release(quint64 token)
{
    const auto lease = m_leases.find(token); if (lease == m_leases.end()) return;
    const auto uuid = lease.value(); m_leases.erase(lease);
    auto entry = m_entries.find(uuid); if (entry == m_entries.end() || --entry->consumers) return;
    entry->touched = ++m_order;
    if (entry->phase == Phase::Ready || entry->phase == Phase::Failed) return; // bounded retained hit/failure
    const auto job = m_jobs.value(entry->job); const auto ticket = entry->job;
    m_entries.erase(entry);
    const QPointer<AvatarImages> guard(this);
    if (ticket && job.phase == JobPhase::Network) {
        m_jobs.remove(ticket);
        if (job.transfer && m_files) m_files->cancel(job.transfer);
        if (!guard) return;
    } // A queued/active decode is bounded but not forcibly interrupted; its result is now unowned.
    drain();
}
void AvatarImages::drain()
{
    if (m_draining || m_stopping || !scopeCurrent() || !m_files) return;
    m_draining = true;
    const QPointer<AvatarImages> guard(this);
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
void AvatarImages::downloaded(const QString &scope, quint64 job, const FileResult &result)
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
        const QPointer<AvatarImages> guard(this); notifyRevision();
        if (guard) drain();
        return;
    }
    ticket->phase = JobPhase::Decode; ticket->transfer = 0; entry->phase = Phase::Decode;
    emit decodeRequested(scope, job, result.bytes);
    notifyRevision();
}
void AvatarImages::decoded(const QString &scope, quint64 job, const QImage &image)
{
    if (m_stopping) return;
    auto ticket = m_jobs.find(job); if (ticket == m_jobs.end() || ticket->phase != JobPhase::Decode) return;
    const auto uuid = ticket->uuid; m_jobs.erase(ticket);
    if (!scopeCurrent()) {
        const QPointer<AvatarImages> guard(this); refreshScope();
        if (guard) drain();
        return;
    }
    auto entry = m_entries.find(uuid);
    if (scope == m_scope && entry != m_entries.end() && entry->job == job && entry->consumers) {
        entry->job = 0; entry->phase = image.isNull() ? Phase::Failed : Phase::Ready;
        if (!image.isNull()) { QMutexLocker lock(&m_images->mutex); m_images->images.insert(cacheKey(uuid), image); }
        const QPointer<AvatarImages> guard(this); notifyRevision();
        if (!guard) return;
    }
    drain();
}
QString AvatarImages::imageUrl(const QString &urn) const
{
    const auto uuid = uuidFor(urn); const auto entry = m_entries.constFind(uuid);
    return scopeCurrent() && !uuid.isEmpty() && entry != m_entries.cend() && entry->phase == Phase::Ready
        ? "image://avatars/" + cacheKey(uuid) : QString{};
}
bool AvatarImages::failed(const QString &urn) const
{
    const auto entry = m_entries.constFind(uuidFor(urn));
    return scopeCurrent() && entry != m_entries.cend()
        && (entry->phase == Phase::Failed || entry->phase == Phase::TransportFailed);
}
AvatarImages::Stats AvatarImages::stats() const
{
    Stats result; result.entries = m_entries.size(); result.leases = m_leases.size(); result.pipelineJobs = m_jobs.size();
    for (const auto &entry : m_entries) if (entry.phase == Phase::Queued) ++result.queued;
    for (const auto &job : m_jobs) if (job.phase == JobPhase::Decode) ++result.decodeJobs;
    QMutexLocker lock(&m_images->mutex); result.thumbnails = m_images->images.size();
    for (const auto &image : m_images->images) result.thumbnailBytes += image.sizeInBytes();
    return result;
}
QQuickImageProvider *AvatarImages::createImageProvider() const { return new CachedAvatarProvider(m_images); }
} // namespace workspace
#include "avatar-images.moc"
