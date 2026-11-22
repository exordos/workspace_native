#include "media-viewer.h"

#include "file-client.h"
#include "heif-preview.h"
#include "native-controller.h"
#include "native-markdown.h"

#include <QFileInfo>
#include <QFile>
#include <QImageWriter>
#include <QPair>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVector>

namespace workspace {

namespace {
bool stagedFileLooksHeif(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return heifSniffFtyp(file.read(kHeifFtypHeaderLimit));
}

// Test seam mirroring MessageImages' decode gate (#670 finding 2): stalls the
// serial HEIC render so the watchdog path is deterministic in tests.
std::function<void()> g_heifRenderGateForTests;
} // namespace

// Decode-thread render for HEIC image slots (issue #638 sec 4.2): the staged
// ORIGINAL stays untouched on disk (download-by-uuid saves it verbatim); this
// writes a PNG render next to it and the slot's display URL points at the
// render instead. Any failure (oversize above the reference 25 MiB cap,
// corrupt container, pixel budget, missing plugin) yields an empty path so
// the slot lands on the display-error state with download still enabled.
class MediaViewerHeifRenderer final : public QObject {
    Q_OBJECT
public slots:
    void render(int slot, quint64 generation, const QString &path, const QString &outPath)
    {
        // #670 re-review: the bounded wait measures from the moment this
        // serial worker takes the render, not from the download; queued
        // renders must not burn their timeout behind an earlier job.
        emit started(slot, generation);
        bool ok = false;
        if (!QThread::currentThread()->isInterruptionRequested()) {
            if (g_heifRenderGateForTests)
                g_heifRenderGateForTests();
            const QFileInfo info(path);
            if (info.exists() && info.size() > 0 && info.size() <= kHeifMaxFileBytes) {
                QFile file(path);
                QImage image;
                QString error;
                if (file.open(QIODevice::ReadOnly)
                    && heifDecodeRgba(file.readAll(), image, error)) {
                    QImageWriter writer(outPath, "PNG");
                    ok = writer.write(image);
                }
            }
        }
        QMetaObject::invokeMethod(this, [this, slot, generation, outPath, ok] {
            emit rendered(slot, generation, ok ? outPath : QString{});
        }, Qt::QueuedConnection);
    }
signals:
    void started(int slot, quint64 generation);
    void rendered(int slot, quint64 generation, const QString &displayPath);
};

MediaViewerModel::MediaViewerModel(NativeController *controller, QObject *parent)
    : QObject(parent), m_controller(controller), m_markdown(std::make_unique<NativeMarkdown>(this))
{
    auto *renderer = new MediaViewerHeifRenderer;
    m_decoder = renderer; renderer->moveToThread(&m_decodeThread);
    m_decodeThread.setObjectName("mediaViewerHeifRenderer");
    connect(&m_decodeThread, &QThread::finished, renderer, &QObject::deleteLater);
    connect(this, &MediaViewerModel::heifRenderRequested, renderer, &MediaViewerHeifRenderer::render,
        Qt::QueuedConnection);
    connect(renderer, &MediaViewerHeifRenderer::started, this, &MediaViewerModel::renderStarted,
        Qt::QueuedConnection);
    connect(renderer, &MediaViewerHeifRenderer::rendered, this, &MediaViewerModel::heifRendered,
        Qt::QueuedConnection);
    m_decodeThread.start();
    if (controller) {
        // The reference viewer auto-closes when its conversation scope changes
        // (workspace-media-viewer.hook.ts:129-135); session end closes too.
        connect(controller, &NativeController::stateChanged, this, [this] {
            if (!m_open || !m_controller)
                return;
            if (!m_controller->authenticated() || m_controller->ownerKey() != m_ownerKey
                || m_controller->selectedStream() != m_stream
                || m_controller->selectedTopic() != m_topic)
                closeSession();
        });
    }
}

MediaViewerModel::~MediaViewerModel()
{
    m_stopping = true;
    if (m_decoder) disconnect(m_decoder, nullptr, this, nullptr);
    closeSession();
    m_decodeThread.requestInterruption(); m_decodeThread.quit(); m_decodeThread.wait();
}

void MediaViewerModel::setFileClient(FileClient *files)
{
    if (m_files == files)
        return;
    // A retired file service can no longer answer in-flight slot requests.
    closeSession();
    m_files = files;
}

void MediaViewerModel::setHeifRenderGateForTests(std::function<void()> gate)
{
    g_heifRenderGateForTests = std::move(gate);
}

void MediaViewerModel::setHeifRenderTimeoutMsForTests(int ms)
{
    m_renderTimeoutMs = ms;
}

void MediaViewerModel::setHeifQueueTimeoutMsForTests(int ms)
{
    m_queueTimeoutMs = ms;
}

void MediaViewerModel::renderStarted(int slot, quint64 generation)
{
    if (m_stopping || generation != m_generation || slot < 0 || slot >= m_slots.size())
        return; // retired or replaced session: the existing fences discard the render
    auto &target = m_slots[slot];
    if (target.state != QLatin1String("loading"))
        return; // already settled (queue bound fired): the late start is fenced
    cancelRenderQueueWatchdog(target); // the worker took the render: queue bound is obsolete
    armHeifRenderWatchdog(slot); // the bounded wait measures the render, not the queue
}

void MediaViewerModel::armHeifQueueWatchdog(int slot)
{
    if (m_queueTimeoutMs <= 0 || slot < 0 || slot >= m_slots.size())
        return;
    auto &target = m_slots[slot];
    cancelRenderQueueWatchdog(target);
    target.queuePending = true;
    const auto generation = m_generation;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    target.queueTimer = timer;
    connect(timer, &QTimer::timeout, this, [this, timer, slot, generation] {
        heifQueueTimeoutExpired(timer, slot, generation);
    });
    timer->start(m_queueTimeoutMs);
}

void MediaViewerModel::cancelRenderQueueWatchdog(Slot &slot)
{
    slot.queuePending = false;
    if (!slot.queueTimer)
        return;
    slot.queueTimer->stop();
    slot.queueTimer->disconnect();
    slot.queueTimer->deleteLater();
    slot.queueTimer = nullptr;
}

void MediaViewerModel::heifQueueTimeoutExpired(QTimer *timer, int slot, quint64 generation)
{
    timer->deleteLater();
    if (m_stopping || generation != m_generation || slot < 0 || slot >= m_slots.size())
        return;
    auto &target = m_slots[slot];
    if (!target.queuePending) // the worker took this render: the render watchdog owns it
        return;
    target.queuePending = false;
    target.queueTimer = nullptr;
    // #670 re-review round: the serial worker cannot be terminated, so a
    // render stuck mid-decode would otherwise strand every queued slot in
    // loading forever. A queued render that never gets worker start settles
    // display-error just like a failed render (uuid stays downloadable);
    // its late start/render is fenced by renderStarted/heifRendered.
    if (target.state == QLatin1String("loading")) {
        target.state = QStringLiteral("display-error");
        target.url.clear();
        emit mediaViewerChanged();
    }
}

void MediaViewerModel::armHeifRenderWatchdog(int slot)
{
    if (m_renderTimeoutMs <= 0 || slot < 0 || slot >= m_slots.size())
        return;
    auto &target = m_slots[slot];
    cancelRenderWatchdog(target);
    target.renderPending = true;
    const auto generation = m_generation;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    target.renderTimer = timer;
    connect(timer, &QTimer::timeout, this, [this, timer, slot, generation] {
        heifRenderTimeoutExpired(timer, slot, generation);
    });
    timer->start(m_renderTimeoutMs);
}

void MediaViewerModel::cancelRenderWatchdog(Slot &slot)
{
    slot.renderPending = false;
    if (!slot.renderTimer)
        return;
    slot.renderTimer->stop();
    slot.renderTimer->disconnect();
    slot.renderTimer->deleteLater();
    slot.renderTimer = nullptr;
}

void MediaViewerModel::heifRenderTimeoutExpired(QTimer *timer, int slot, quint64 generation)
{
    timer->deleteLater();
    // A retired or replaced session (close, generation bump, newer slot) keeps
    // the existing fences: the late render is discarded there, nothing to do.
    if (m_stopping || generation != m_generation || slot < 0 || slot >= m_slots.size())
        return;
    auto &target = m_slots[slot];
    if (!target.renderPending) // the render already settled this slot
        return;
    target.renderPending = false;
    target.renderTimer = nullptr;
    // #670 finding 2: a stalled render must not leave the slot loading
    // forever; it lands on display-error exactly like a failed render
    // (uuid stays downloadable - reference replaceItem). The late worker
    // result is then discarded by heifRendered's state fence.
    if (target.state == QLatin1String("loading")) {
        target.state = QStringLiteral("display-error");
        target.url.clear();
        emit mediaViewerChanged();
    }
}

QVariantList MediaViewerModel::items() const
{
    QVariantList result;
    for (const auto &slot : m_slots)
        result.append(QVariantMap{{"uuid", slot.uuid}, {"kind", slot.kind}, {"state", slot.state},
            {"url", slot.url}, {"name", slot.name}});
    return result;
}

bool MediaViewerModel::openFromMessages(const QVariantList &messages, const QString &fileUuid)
{
    if (fileUuid.isEmpty())
        return false;
    // Collection contract (workspace-message-list-media.lib.ts:20-31,44-66):
    // markdown rows only, image and video workspace references in document
    // order, deduped by file uuid keeping the first occurrence.
    QVector<QPair<QString, QString>> gallery;
    QSet<QString> seen;
    for (const auto &entry : messages) {
        const auto row = entry.toMap();
        if (row.value("kind").toString() != QLatin1String("markdown"))
            continue;
        const auto text = row.contains("displayContent")
            ? row.value("displayContent").toString()
            : row.value("content").toString();
        for (const auto &part : m_markdown->mediaReferences(text)) {
            const auto reference = part.toMap();
            const auto kind = reference.value("kind").toString();
            const auto uuid = reference.value("id").toString();
            if (kind != QLatin1String("image") && kind != QLatin1String("video"))
                continue;
            if (uuid.isEmpty() || seen.contains(uuid))
                continue;
            seen.insert(uuid);
            gallery.append(qMakePair(uuid, kind));
        }
    }
    int start = -1;
    for (int slot = 0; slot < gallery.size(); ++slot) {
        if (gallery.at(slot).first == fileUuid) {
            start = slot;
            break;
        }
    }
    if (gallery.isEmpty() || start < 0)
        return false;
    closeSession();
    m_directory = std::make_unique<QTemporaryDir>();
    ++m_generation;
    m_slots.clear();
    for (const auto &entry : gallery)
        m_slots.append(Slot{entry.first, entry.second, QStringLiteral("loading"),
            QString{}, QString{}, 0, false});
    m_index = qBound(0, start, m_slots.size() - 1);
    m_open = true;
    if (m_controller) {
        m_ownerKey = m_controller->ownerKey();
        m_stream = m_controller->selectedStream();
        m_topic = m_controller->selectedTopic();
        m_authenticated = m_controller->authenticated();
    }
    emit mediaViewerChanged();
    // The clicked slot stages first, then every image slot; video slots other
    // than the clicked one stay loading until navigation reaches them
    // (workspace-media-viewer.hook.ts:205-212).
    startSlot(m_index);
    for (int slot = 0; slot < m_slots.size(); ++slot) {
        if (slot != m_index && m_slots[slot].kind == QLatin1String("image"))
            startSlot(slot);
    }
    return true;
}

void MediaViewerModel::goTo(int index)
{
    if (!m_open || m_slots.isEmpty())
        return;
    // The reference clamps goTo to [0, len-1] (media-viewer.model.ts:100-106).
    const int target = qBound(0, index, m_slots.size() - 1);
    if (target == m_index)
        return;
    m_index = target;
    emit mediaViewerChanged();
    startPendingSlot(m_index);
}

void MediaViewerModel::next()
{
    if (m_open && m_index + 1 < m_slots.size()) {
        ++m_index;
        emit mediaViewerChanged();
        startPendingSlot(m_index);
    }
}

void MediaViewerModel::prev()
{
    if (m_open && m_index > 0) {
        --m_index;
        emit mediaViewerChanged();
        startPendingSlot(m_index);
    }
}

void MediaViewerModel::close()
{
    closeSession();
}

bool MediaViewerModel::copyCurrentImage()
{
    // Toolbar copy in the reference renders only for image items
    // (media-viewer-overlay.ui.tsx:118) and hands the displayed bytes to
    // writeImage (media-viewer-actions.lib.ts:144-151). Here the slot's staged
    // session bytes are that payload (see the deviations section of
    // docs/NATIVE_DESKTOP_CLIPBOARD_IMAGE.md); a missing or failed slot is the
    // counterpart of the swallowed null-blob rejection: silent false.
    if (!m_open || m_index < 0 || m_index >= m_slots.size())
        return false;
    const auto &slot = m_slots.at(m_index);
    if (slot.kind != QLatin1String("image") || slot.state != QLatin1String("ready"))
        return false;
    const QUrl url(slot.url);
    if (!url.isLocalFile())
        return false;
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // Fix round R1 (cid=4577117373): read through the shared bounded helper so
    // a staged slot file over the clipboard cap is rejected without ever being
    // loaded whole into memory.
    return NativeController::clipboardImageBytes(NativeController::readClipboardBounded(file));
}

void MediaViewerModel::markSlotDisplayError(int index)
{
    if (!m_open || index < 0 || index >= m_slots.size())
        return;
    auto &target = m_slots[index];
    // The reference only replaces the current ready item
    // (media-viewer-overlay.ui.tsx:75-90).
    if (target.state != QLatin1String("ready"))
        return;
    target.state = QStringLiteral("display-error");
    target.url.clear(); // The uuid stays downloadable through the slot entry.
    emit mediaViewerChanged();
}

void MediaViewerModel::closeSession()
{
    if (!m_open && m_slots.isEmpty())
        return;
    ++m_generation;
    QList<quint64> operations;
    for (auto &slot : m_slots) {
        cancelRenderWatchdog(slot);
        cancelRenderQueueWatchdog(slot);
    }
    for (const auto &slot : m_slots) {
        if (slot.operation != 0)
            operations.append(slot.operation);
    }
    m_open = false;
    m_index = 0;
    m_slots.clear();
    m_directory.reset(); // Removes the staged bytes (the blob-revocation analogue).
    if (m_files) {
        for (const auto operation : operations)
            m_files->cancel(operation);
    }
    emit mediaViewerChanged();
}

void MediaViewerModel::startPendingSlot(int slot)
{
    // Non-clicked video slots start the moment navigation reaches them
    // (workspace-media-viewer.hook.ts:137-151,260-283).
    if (!m_open || slot < 0 || slot >= m_slots.size())
        return;
    const auto &target = m_slots[slot];
    if (target.started || target.state != QLatin1String("loading"))
        return;
    startSlot(slot);
}

void MediaViewerModel::startSlot(int slot)
{
    if (slot < 0 || slot >= m_slots.size())
        return;
    m_slots[slot].started = true;
    if (!m_files || !m_directory || !m_directory->isValid()) {
        m_slots[slot].state = QStringLiteral("load-error");
        emit mediaViewerChanged();
        return;
    }
    const auto generation = m_generation;
    const auto destination = m_directory->filePath(QStringLiteral("slot-%1").arg(slot));
    const QPointer<MediaViewerModel> self(this);
    const auto uuid = m_slots[slot].uuid;
    const auto operation = m_files->download(uuid, destination, [self, slot, generation](const FileResult &result) {
        if (self) self->slotDownloaded(slot, generation, result);
    });
    // Synchronous local failures complete before the id returns; a retired
    // session (close during this call) already cleared the slots.
    if (slot < m_slots.size())
        m_slots[slot].operation = operation;
}

void MediaViewerModel::slotDownloaded(int slot, quint64 generation, const FileResult &result)
{
    if (!m_open || generation != m_generation || slot >= m_slots.size())
        return;
    auto &target = m_slots[slot];
    if (result.ok()) {
        target.name = result.metadata.name.isEmpty() ? QFileInfo(result.path).fileName() : result.metadata.name;
        if (target.kind == QLatin1String("image") && stagedFileLooksHeif(result.path)) {
            // Issue #638 sec 4.2: the ISO-BMFF original cannot be rendered by
            // QML Image, so the slot stays loading while the worker writes a
            // decoded PNG render; a failed render lands on display-error
            // (reference replaceItem), download-by-uuid still saves the
            // untouched original staged here.
            const auto outPath = m_directory && m_directory->isValid()
                ? m_directory->filePath(QStringLiteral("slot-%1-heif.png").arg(slot)) : QString{};
            if (!outPath.isEmpty()) {
                // The render wait is bounded in two separate budgets: the
                // queue bound below measures until the serial worker takes
                // the render, the render watchdog (armed by renderStarted)
                // measures the render itself (#670 re-review).
                armHeifQueueWatchdog(slot);
                emit heifRenderRequested(slot, generation, result.path, outPath);
                emit mediaViewerChanged(); // the metadata name is visible while rendering
                return;
            }
            target.state = QStringLiteral("display-error");
            target.url.clear();
        } else {
            target.state = QStringLiteral("ready");
            target.url = QUrl::fromLocalFile(result.path).toString();
        }
    } else {
        target.state = QStringLiteral("load-error");
        target.url.clear();
        target.name.clear();
    }
    emit mediaViewerChanged();
}

void MediaViewerModel::heifRendered(int slot, quint64 generation, const QString &displayPath)
{
    if (m_stopping || generation != m_generation || slot < 0 || slot >= m_slots.size())
        return;
    auto &target = m_slots[slot];
    cancelRenderWatchdog(target); // the render reported: its watchdog is obsolete
    cancelRenderQueueWatchdog(target);
    if (target.state != QLatin1String("loading"))
        return; // replaced meanwhile (close, explicit mark): the late render is discarded
    if (displayPath.isEmpty())
        target.state = QStringLiteral("display-error"); // uuid stays downloadable (reference replaceItem)
    else {
        target.state = QStringLiteral("ready");
        target.url = QUrl::fromLocalFile(displayPath).toString();
    }
    emit mediaViewerChanged();
}

} // namespace workspace
#include "media-viewer.moc"
