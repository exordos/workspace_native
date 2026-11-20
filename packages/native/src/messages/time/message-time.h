#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>

namespace workspace::message_time {

QString relativeLabel(const QDateTime &created, const QLocale &locale, const QDate &today, const QString &yesterdayLabel);
QString datedLabel(const QDateTime &created, const QLocale &locale, const QDate &today);

} // namespace workspace::message_time
