#pragma once

#include "../mock-server.h"

#include <QDateTime>
#include <QUrlQuery>

namespace workspace::test {

inline const QString workCalendarId = "81000000-0000-4000-8000-000000000001";
inline const QString personalCalendarId = "81000000-0000-4000-8000-000000000002";

inline QJsonArray calendarList()
{
    return {QJsonObject{{"uuid", workCalendarId}, {"project_id", projectId}, {"name", "Team calendar"}},
            QJsonObject{{"uuid", personalCalendarId}, {"project_id", projectId}, {"name", "On call"}}};
}

inline QJsonObject calendarEvent(const QString &uuid, const QString &day, const QString &start,
                                 const QString &end, const QString &title,
                                 const QString &calendar = workCalendarId, const QString &location = {})
{
    const auto instant = [&](const QString &time) {
        return QDateTime::fromString(day + "T" + time + ":00", Qt::ISODate)
            .toUTC().toString("yyyy-MM-dd'T'HH:mm:ss") + ".123456Z";
    };
    return {{"uuid", uuid}, {"project_id", projectId}, {"calendar_uuid", calendar},
            {"payload", QJsonObject{{"kind", "event"}, {"title", title}, {"location", location},
                                    {"starts_at", instant(start)}, {"ends_at", instant(end)}}}};
}

inline QString calendarFirstDay(const QUrlQuery &query)
{
    return QDateTime::fromString(query.queryItemValue("ends_after"), Qt::ISODate)
        .toLocalTime().date().toString(Qt::ISODate);
}

inline QString calendarLastDay(const QUrlQuery &query)
{
    return QDateTime::fromString(query.queryItemValue("starts_before"), Qt::ISODate)
        .toLocalTime().date().addDays(-1).toString(Qt::ISODate);
}

} // namespace workspace::test
