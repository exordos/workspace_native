#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QVariantMap>

namespace workspace {

// Qt's document parser is used with raw HTML disabled. Rendering never loads a
// resource; attachments are resolved separately through the authenticated API.
class NativeMarkdown final : public QObject {
    Q_OBJECT
public:
    explicit NativeMarkdown(QObject *parent = nullptr) : QObject(parent) {}
    // hideMedia drops every image/video reference from the rendered document
    // (reference hiddenWorkspaceMediaFileUuids, issue #217): the body keeps its
    // text and loses the media label the renderer would otherwise print.
    Q_INVOKABLE QVariantMap render(const QString &markdown, bool hideMedia = false);
    Q_INVOKABLE QVariantMap reference(const QString &target) const;
    Q_INVOKABLE bool openExternal(const QString &target) const;
    // Workspace image/video references found in a message body in document
    // order (media gallery collection, docs/NATIVE_DESKTOP_MEDIA_VIEWER_VIDEO.md).
    QVariantList mediaReferences(const QString &markdown) const;
    // Session-local edit inventory; verbatim source survives extraction/save.
    QVariantMap editContent(const QString &markdown) const;
    QString removeEditAttachment(const QString &markdown, const QString &attachment) const;
    static QVariantList updateEditAttachments(const QString &markdown, const QVariantList &attachments);
    static bool startsWithParagraph(const QString &markdown, const QString &paragraph);
    static QString prependEditAttachments(const QString &markdown, const QVariantList &attachments);

private:
    QHash<QString, QVariantMap> m_cache;
    QQueue<QString> m_order;
};

}
