#pragma once

#include <QList>
#include <QPointer>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantList>

#include <functional>
#include <memory>

#include "heif-preview.h"

class QTimer;

class QTemporaryDir;

namespace workspace {

struct FileResult;
class FileClient;
class NativeController;
class NativeMarkdown;

// Media viewer session state (docs/NATIVE_DESKTOP_MEDIA_VIEWER.md): a snapshot
// of image slots collected from the rendered message rows, mirroring the
// reference collection contract (markdown rows only, document order, deduped
// by file uuid keeping the first occurrence). Each slot stages its full-size
// bytes through the authenticated FileClient pipeline into a per-session
// temporary directory that is removed when the session closes.
class MediaViewerModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ open NOTIFY mediaViewerChanged)
    Q_PROPERTY(QVariantList items READ items NOTIFY mediaViewerChanged)
    Q_PROPERTY(int index READ index NOTIFY mediaViewerChanged)
    Q_PROPERTY(int count READ count NOTIFY mediaViewerChanged)
    Q_PROPERTY(bool canPrev READ canPrev NOTIFY mediaViewerChanged)
    Q_PROPERTY(bool canNext READ canNext NOTIFY mediaViewerChanged)
public:
    explicit MediaViewerModel(NativeController *controller, QObject *parent = nullptr);
    ~MediaViewerModel() override;

    bool open() const { return m_open; }
    int index() const { return m_index; }
    int count() const { return m_slots.size(); }
    bool canPrev() const { return m_index > 0; }
    bool canNext() const { return m_index + 1 < m_slots.size(); }
    QVariantList items() const;

    // Owned elsewhere; attached alongside the controller's useFiles().
    void setFileClient(FileClient *files);

    // Test seams mirroring MessageImages' decode gate (#670 finding 2): the
    // gate stalls the serial HEIC render, the timeout shortens the render
    // watchdog. Neither changes behaviour with defaults installed.
    static void setHeifRenderGateForTests(std::function<void()> gate);
    void setHeifRenderTimeoutMsForTests(int ms);
    // #670 re-review round: a queued HEIC render is bounded separately from
    // the per-render watchdog, because the watchdog cannot kill the serial
    // worker: a render stuck mid-decode must not strand later slots in
    // loading forever. Neither knob changes behaviour with defaults installed.
    void setHeifQueueTimeoutMsForTests(int ms);

    // Returns false (viewer untouched) when there is no image slot or the
    // requested file is not part of the collected gallery.
    Q_INVOKABLE bool openFromMessages(const QVariantList &messages, const QString &fileUuid);
    Q_INVOKABLE void goTo(int index);
    Q_INVOKABLE void next();
    Q_INVOKABLE void prev();
    Q_INVOKABLE void close();
    // Issue #79: toolbar copy for image slots
    // (docs/NATIVE_DESKTOP_CLIPBOARD_IMAGE.md).
    Q_INVOKABLE bool copyCurrentImage();
    // Red test stub (issue #203): body intentionally empty until the
    // reference replace contract is implemented.
    Q_INVOKABLE void markSlotDisplayError(int index);

signals:
    void mediaViewerChanged();

private:
    struct Slot {
        QString uuid;
        QString kind;  // "image" | "video" (media-viewer.types.ts:8)
        QString state; // "loading" | "ready" | "load-error" | "display-error" (types.ts:9)
        QString url;   // file:// URL of the staged full-size bytes once ready
        QString name;  // server-provided file name once metadata resolved
        quint64 operation = 0;
        bool started = false; // video slots other than the opened one load lazily
        // #670 finding 2: the HEIC render wait is bounded like the reference
        // decode timeout; a stalled serial render settles display-error and a
        // late render is discarded by the existing fences.
        QTimer *renderTimer = nullptr;
        bool renderPending = false;
        // #670 re-review round: the wait for the serial worker to even TAKE
        // this render is bounded on its own (the render watchdog only starts
        // at worker start and the worker can be stuck inside an earlier job).
        QTimer *queueTimer = nullptr;
        bool queuePending = false;
    };

    void closeSession();
    void startSlot(int slot);
    void startPendingSlot(int slot);
    void slotDownloaded(int slot, quint64 generation, const FileResult &result);
    void heifRendered(int slot, quint64 generation, const QString &displayPath);
    void renderStarted(int slot, quint64 generation);
    void armHeifRenderWatchdog(int slot);
    void cancelRenderWatchdog(Slot &slot);
    void heifRenderTimeoutExpired(QTimer *timer, int slot, quint64 generation);
    void armHeifQueueWatchdog(int slot);
    void cancelRenderQueueWatchdog(Slot &slot);
    void heifQueueTimeoutExpired(QTimer *timer, int slot, quint64 generation);
    QPointer<NativeController> m_controller;
    QPointer<FileClient> m_files;
    std::unique_ptr<NativeMarkdown> m_markdown;
    std::unique_ptr<QTemporaryDir> m_directory;
    QList<Slot> m_slots;
    bool m_open = false;
    int m_index = 0;
    quint64 m_generation = 0;
    QString m_ownerKey, m_stream, m_topic;
    bool m_authenticated = false;
    // Issue #638 sec 4.2: image slots whose staged bytes are HEIC get their
    // display render decoded on a dedicated worker thread (QML Image cannot
    // read ISO-BMFF); results are fenced by m_generation like the downloads.
    QThread m_decodeThread;
    QPointer<QObject> m_decoder;
    int m_renderTimeoutMs = kHeifDecodeTimeoutMs; // #670 finding 2
    // Queue bound: how long a HEIC render may wait for the serial worker to
    // take it. Deliberately larger than the per-render bound - queueing is
    // legal work (N gallery slots), only an UNBOUNDED queue is the bug.
    int m_queueTimeoutMs = kHeifDecodeTimeoutMs * 4; // #670 re-review round
    bool m_stopping = false;
    Q_SIGNAL void heifRenderRequested(int slot, quint64 generation, const QString &path,
        const QString &outPath);
};

} // namespace workspace
