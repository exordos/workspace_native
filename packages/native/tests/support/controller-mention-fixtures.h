#pragma once

#include "native-test-harness.h"

inline QJsonObject mentionMessage(int number, bool read = false, bool own = false)
{
    const auto id = QStringLiteral("77777777-7777-4777-8777-")
        + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
    auto row = message(QStringLiteral("Mention %1").arg(number), id);
    row.insert("read", read);
    row.insert("is_own", own);
    row.insert("mentioned", true);
    row.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                     .addSecs(number).toString(Qt::ISODateWithMs));
    return row;
}

// Serves the mentioned=true message page like the Workspace API:
// read=false narrows to unread rows, page_limit caps the page and the
// continuation cursor rides the X-Pagination-Marker header.
inline Response mentionPageResponse(const QMap<int, QJsonObject> &rows, const Request &request)
{
    const auto query = QUrlQuery(request.target);
    const bool unreadOnly = query.queryItemValue("read") == "false";
    const int limit = qMax(1, query.queryItemValue("page_limit").toInt());
    const auto marker = query.queryItemValue("page_marker");
    const int boundary = marker.isEmpty() ? std::numeric_limits<int>::max()
                                         : marker.right(12).toInt(nullptr, 16);
    QJsonArray items;
    bool more = false;
    for (auto it = rows.cend(); it != rows.cbegin();) {
        --it;
        if (it.key() >= boundary)
            continue;
        if (unreadOnly && it.value().value("read").toBool())
            continue;
        if (items.size() == limit) {
            more = true;
            break;
        }
        items.append(it.value());
    }
    Response response{items, 200, 0, {}};
    if (more)
        response.headers.insert("X-Pagination-Marker",
            items.last().toObject().value("uuid").toString().toUtf8());
    return response;
}

inline QString mentionUuid(const QString &tag)
{
    return QStringLiteral("51000000-0000-4000-8000-") + tag + QStringLiteral("0000000000").left(12 - tag.size());
}
