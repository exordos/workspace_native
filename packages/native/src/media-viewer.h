#pragma once

#include <QList>
#include <QPointer>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <memory>

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

    // Returns false (viewer untouched) when there is no image slot or the
    // requested file is not part of the collected gallery.
    Q_INVOKABLE bool openFromMessages(const QVariantList &messages, const QString &fileUuid);
    Q_INVOKABLE void goTo(int index);
    Q_INVOKABLE void next();
    Q_INVOKABLE void prev();
    Q_INVOKABLE void close();

signals:
    void mediaViewerChanged();

private:
    struct Slot {
        QString uuid;
        QString state; // "loading" | "ready" | "error"
        QString url;   // file:// URL of the staged full-size bytes once ready
        QString name;  // server-provided file name once metadata resolved
        quint64 operation = 0;
    };

    void closeSession();
    void startSlot(int slot);

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
};

} // namespace workspace
