#include "message-time.h"

namespace workspace::message_time {

QString relativeLabel(const QDateTime &created, const QLocale &locale,
    const QDate &today, const QString &yesterdayLabel)
{
    // workspace_ui 61dacef shared/lib/datetime.lib.ts:65-82 formatMessageTimeRelative:
    // the same local calendar day is a manual 24 h HH:mm (never Intl), the previous local
    // day is the chat.yesterday label with no clock time, anything older is the compact
    // day + short month style, which carries no year at all.
    if (!created.isValid()) return {};
    const auto local = created.toLocalTime();
    if (local.date() == today) return local.toString(QStringLiteral("HH:mm"));
    if (local.date() == today.addDays(-1)) return yesterdayLabel;
    return locale.toString(local.date(), locale.language() == QLocale::Russian
        ? QStringLiteral("d MMM") : QStringLiteral("MMM d"));
}

QString datedLabel(const QDateTime &created, const QLocale &locale, const QDate &today)
{
    // datetime.lib.ts:86-96 formatMessageTimeWithDate: the same local day is a
    // manual 24 h HH:mm; anything older is the locale dated style (2-digit day,
    // short month, numeric year) followed by a space and HH:mm.
    if (!created.isValid())
        return {};
    const auto local = created.toLocalTime();
    if (local.date() == today)
        return local.toString(QStringLiteral("HH:mm"));
    const auto datePart = locale.language() == QLocale::Russian
        ? locale.toString(local.date(), QStringLiteral("dd MMM yyyy 'г.'"))
        : locale.toString(local.date(), QStringLiteral("MMM dd, yyyy"));
    return datePart + QLatin1Char(' ') + local.toString(QStringLiteral("HH:mm"));
}

} // namespace workspace::message_time
