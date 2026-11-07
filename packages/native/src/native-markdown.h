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
    Q_INVOKABLE QVariantMap render(const QString &markdown);
    Q_INVOKABLE QVariantMap reference(const QString &target) const;
    Q_INVOKABLE bool openExternal(const QString &target) const;

private:
    QHash<QString, QVariantMap> m_cache;
    QQueue<QString> m_order;
};

}
