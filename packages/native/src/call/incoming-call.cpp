#include "call/incoming-call.h"

#include "call/jitsi-call.h"

#include <QDateTime>

namespace workspace {

namespace {
QString trimmedOrEmpty(const QString &value)
{
    return value.trimmed();
}
} // namespace

QVariantMap buildIncomingDmCallInvite(const QVariantMap &input)
{
    // workspace-jitsi-incoming-call.lib.ts:40-57 (message filters).
    if (input.value(QStringLiteral("messageRead")).toBool()
        || input.value(QStringLiteral("messageOwn")).toBool()
        || input.value(QStringLiteral("authorUuid")).toString()
            == input.value(QStringLiteral("currentUserUuid")).toString())
        return {};
    if (!input.value(QStringLiteral("streamKnown")).toBool()
        || !input.value(QStringLiteral("streamPrivate")).toBool()
        || trimmedOrEmpty(input.value(QStringLiteral("streamDirectUser")).toString()).isEmpty())
        return {};
    if (input.value(QStringLiteral("payloadKind")).toString()
            != QStringLiteral("markdown"))
        return {};
    const auto meetingUrl = getJitsiMeetingUrl(input.value(QStringLiteral("content")).toString(),
        input.value(QStringLiteral("meetUrl")).toString());
    if (meetingUrl.isEmpty())
        return {};

    // resolveCallerName (lib.ts:23-34): display name -> stream name ->
    // first 8 chars of the author uuid.
    const auto streamName = trimmedOrEmpty(input.value(QStringLiteral("streamName")).toString());
    auto callerName = trimmedOrEmpty(input.value(QStringLiteral("authorDisplayName")).toString());
    if (callerName.isEmpty())
        callerName = streamName;
    if (callerName.isEmpty())
        callerName = trimmedOrEmpty(input.value(QStringLiteral("authorUuid")).toString()).left(8);

    // Invite fields per lib.ts:58-72; empty optionals stay absent, matching
    // trimToOptional ?? undefined.
    QVariantMap invite;
    invite.insert(QStringLiteral("messageId"), input.value(QStringLiteral("messageId")));
    invite.insert(QStringLiteral("meetingUrl"), meetingUrl);
    invite.insert(QStringLiteral("callerName"), callerName);
    invite.insert(QStringLiteral("locationName"), streamName);
    invite.insert(QStringLiteral("ownerKey"), input.value(QStringLiteral("ownerKey")));
    const auto meetUrl = trimmedOrEmpty(input.value(QStringLiteral("meetUrl")).toString());
    if (!meetUrl.isEmpty())
        invite.insert(QStringLiteral("meetUrl"), meetUrl);
    const auto displayName = trimmedOrEmpty(
        input.value(QStringLiteral("currentUserDisplayName")).toString());
    if (!displayName.isEmpty())
        invite.insert(QStringLiteral("displayName"), displayName);
    const auto avatarUrl = trimmedOrEmpty(
        input.value(QStringLiteral("authorAvatarUrl")).toString());
    if (!avatarUrl.isEmpty())
        invite.insert(QStringLiteral("avatarUrl"), avatarUrl);
    auto timestamp = input.value(QStringLiteral("createdAtMs")).toLongLong();
    if (timestamp <= 0)
        timestamp = QDateTime::currentMSecsSinceEpoch();
    invite.insert(QStringLiteral("timestamp"), timestamp);
    return invite;
}

QString resolveIncomingCallVariant(const QString &raw)
{
    // shared/lib/env.ts:132-135: lowercase, only "compact" is honored, any
    // other value (including the default "large" and garbage) falls back to
    // "large".
    return raw.toLower() == QLatin1String("compact") ? QStringLiteral("compact")
                                                      : QStringLiteral("large");
}

} // namespace workspace
