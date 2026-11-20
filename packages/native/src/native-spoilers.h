#pragma once

#include <QHash>
#include <QSet>
#include <QString>

// Adapt the HTML produced by md4c. The map only contains disclosure links
// generated for this document, keyed by their actual href.
namespace workspace {
struct SpoilerLinks {
    QString prefix;
    QHash<QString, QString> ids;
    QSet<QString> headers;
};
QByteArray renderSpoilers(const QString &markdown, const QByteArray &html,
    const QSet<QString> &opened, bool expandAll, const QString &defaultLabel, SpoilerLinks &links);
}
