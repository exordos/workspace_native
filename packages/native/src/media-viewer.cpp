#include "media-viewer.h"

#include "file-client.h"
#include "native-controller.h"
#include "native-markdown.h"

#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

namespace workspace {

MediaViewerModel::MediaViewerModel(NativeController *controller, QObject *parent)
    : QObject(parent), m_controller(controller), m_markdown(std::make_unique<NativeMarkdown>(this))
{
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
    closeSession();
}

void MediaViewerModel::setFileClient(FileClient *files)
{
    if (m_files == files)
        return;
    // A retired file service can no longer answer in-flight slot requests.
    closeSession();
    m_files = files;
}

QVariantList MediaViewerModel::items() const
{
    QVariantList result;
    for (const auto &slot : m_slots)
        result.append(QVariantMap{{"uuid", slot.uuid}, {"state", slot.state}, {"url", slot.url}, {"name", slot.name}});
    return result;
}

bool MediaViewerModel::openFromMessages(const QVariantList &messages, const QString &fileUuid)
{
    if (fileUuid.isEmpty())
        return false;
    // Collection contract (workspace-message-list-media.lib.ts:34-77):
    // markdown rows only, document order, deduped by file uuid keeping the
    // first occurrence. Image parts are the same sanitized inline-image
    // references the message rows actually render.
    QStringList gallery;
    QSet<QString> seen;
    for (const auto &entry : messages) {
        const auto row = entry.toMap();
        if (row.value("kind").toString() != QLatin1String("markdown"))
            continue;
        const auto text = row.contains("displayContent")
            ? row.value("displayContent").toString()
            : row.value("content").toString();
        const auto parts = m_markdown->render(text).value("parts").toList();
        for (const auto &part : parts) {
            const auto image = part.toMap();
            if (image.value("kind").toString() != QLatin1String("image"))
                continue;
            const auto uuid = image.value("id").toString();
            if (uuid.isEmpty() || seen.contains(uuid))
                continue;
            seen.insert(uuid);
            gallery.append(uuid);
        }
    }
    const int start = gallery.indexOf(fileUuid);
    if (gallery.isEmpty() || start < 0)
        return false;
    closeSession();
    m_directory = std::make_unique<QTemporaryDir>();
    ++m_generation;
    m_slots.clear();
    for (const auto &uuid : gallery)
        m_slots.append(Slot{uuid, QStringLiteral("loading"), QString{}, QString{}, 0});
    m_index = qBound(0, start, m_slots.size() - 1);
    m_open = true;
    if (m_controller) {
        m_ownerKey = m_controller->ownerKey();
        m_stream = m_controller->selectedStream();
        m_topic = m_controller->selectedTopic();
        m_authenticated = m_controller->authenticated();
    }
    emit mediaViewerChanged();
    // The clicked slot stages first; the rest launch immediately afterwards,
    // mirroring the reference parallel image loading
    // (workspace-media-viewer.hook.ts:205-212).
    startSlot(m_index);
    for (int slot = 0; slot < m_slots.size(); ++slot) {
        if (slot != m_index)
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
}

void MediaViewerModel::next()
{
    if (m_open && m_index + 1 < m_slots.size()) {
        ++m_index;
        emit mediaViewerChanged();
    }
}

void MediaViewerModel::prev()
{
    if (m_open && m_index > 0) {
        --m_index;
        emit mediaViewerChanged();
    }
}

void MediaViewerModel::close()
{
    closeSession();
}

void MediaViewerModel::closeSession()
{
    if (!m_open && m_slots.isEmpty())
        return;
    ++m_generation;
    QList<quint64> operations;
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

void MediaViewerModel::startSlot(int slot)
{
    if (slot < 0 || slot >= m_slots.size())
        return;
    if (!m_files || !m_directory || !m_directory->isValid()) {
        m_slots[slot].state = QStringLiteral("error");
        emit mediaViewerChanged();
        return;
    }
    const auto generation = m_generation;
    const auto destination = m_directory->filePath(QStringLiteral("slot-%1").arg(slot));
    const QPointer<MediaViewerModel> self(this);
    const auto uuid = m_slots[slot].uuid;
    const auto operation = m_files->download(uuid, destination, [self, slot, generation](const FileResult &result) {
        if (!self || generation != self->m_generation || slot >= self->m_slots.size())
            return;
        auto &target = self->m_slots[slot];
        if (result.ok()) {
            target.state = QStringLiteral("ready");
            target.url = QUrl::fromLocalFile(result.path).toString();
            target.name = result.metadata.name.isEmpty() ? QFileInfo(result.path).fileName() : result.metadata.name;
        } else {
            target.state = QStringLiteral("error");
            target.url.clear();
            target.name.clear();
        }
        emit self->mediaViewerChanged();
    });
    // Synchronous local failures complete before the id returns; a retired
    // session (close during this call) already cleared the slots.
    if (slot < m_slots.size())
        m_slots[slot].operation = operation;
}

} // namespace workspace
