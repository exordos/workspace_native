#include "call/jitsi-call.h"

#include <QRegularExpression>
#include <QUrl>

namespace {
using namespace workspace;

constexpr auto kPublicHost = "meet.jit.si";

struct EffectiveBase {
    QString baseUrl;
    QString domain;
};

EffectiveBase effective(const QString &meetUrl)
{
    const auto fromSettings = normalizeMeetOrigin(meetUrl);
    if (!fromSettings.isEmpty()) {
        const QUrl url(fromSettings);
        return {fromSettings, url.host().toLower()};
    }
    return {QString::fromLatin1(kDefaultJitsiMeetBase), QString::fromLatin1(kPublicHost)};
}

QString normalizedUrl(const QString &meetingUrl)
{
    // jitsi-call.model.ts normalizeMeetingUrlForCallKey: trim, cut trailing
    // slashes, lowercase scheme/host via the URL parser, re-cut trailing
    // slashes; an unparseable value keeps the trimmed string.
    auto trimmed = meetingUrl.trimmed();
    while (trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const QUrl parsed(trimmed);
    if (!parsed.isValid() || parsed.isRelative())
        return trimmed;
    const auto scheme = parsed.scheme().toLower();
    if (scheme.isEmpty())
        return trimmed;
    QString out = scheme + QLatin1String("://") + parsed.host().toLower();
    if (parsed.port() != -1)
        out += QLatin1Char(':') + QString::number(parsed.port());
    out += parsed.path(QUrl::FullyEncoded);
    if (parsed.hasQuery())
        out += QLatin1Char('?') + parsed.query(QUrl::FullyEncoded);
    if (parsed.hasFragment())
        out += QLatin1Char('#') + parsed.fragment(QUrl::FullyEncoded);
    while (out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}
} // namespace

namespace workspace {

QString normalizeRoomSegment(const QString &value)
{
    // workspace-jitsi-call.lib.ts normalizeRoomSegment.
    auto normalized = value.trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9-]+")), QStringLiteral("-"));
    normalized.replace(QRegularExpression(QStringLiteral("-+")), QStringLiteral("-"));
    while (normalized.startsWith(QLatin1Char('-')))
        normalized.remove(0, 1);
    while (normalized.endsWith(QLatin1Char('-')))
        normalized.chop(1);
    normalized = normalized.toLower();
    return normalized.isEmpty() ? QStringLiteral("room") : normalized;
}

QString buildWorkspaceJitsiRoomName(const QString &organizationId, const QString &projectId,
    const QString &streamUuid, const QString &topicUuid, qint64 nowMs)
{
    const QStringList segments{QStringLiteral("workspace"), organizationId, projectId,
        streamUuid, topicUuid, QString::number(nowMs)};
    QStringList normalized;
    normalized.reserve(segments.size());
    for (const auto &segment : segments)
        normalized.append(normalizeRoomSegment(segment));
    return normalized.join(QLatin1Char('-'));
}

QString normalizeMeetOrigin(const QString &value)
{
    // jitsi-call-settings.model.ts normalizeWorkspaceMeetUrl: http/https
    // origin only; everything else (missing scheme included) is rejected.
    const auto raw = value.trimmed();
    if (raw.isEmpty())
        return QString();
    const QUrl parsed(raw);
    if (!parsed.isValid() || parsed.isRelative() || parsed.host().isEmpty())
        return QString();
    const auto scheme = parsed.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return QString();
    QString origin = scheme + QLatin1String("://") + parsed.host().toLower();
    if (parsed.port() != -1)
        origin += QLatin1Char(':') + QString::number(parsed.port());
    return origin;
}

QString effectiveMeetBase(const QString &meetUrl)
{
    return effective(meetUrl).baseUrl;
}

QString buildJitsiMeetingUrl(const QString &roomName, const QString &meetUrl)
{
    const auto base = effective(meetUrl).baseUrl;
    return base + QLatin1Char('/') + QString::fromUtf8(QUrl::toPercentEncoding(roomName));
}

QVariantMap parseJitsiUrl(const QString &url, const QString &meetUrl)
{
    // jitsi.ts parseJitsiUrl: http(s) on the effective host or meet.jit.si,
    // first path segment percent-decoded as the room name.
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.isRelative())
        return {};
    const auto scheme = parsed.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    const auto host = parsed.host().toLower();
    auto path = parsed.path();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    const auto room = path.split(QLatin1Char('/')).value(0);
    if (room.isEmpty())
        return {};
    const auto eff = effective(meetUrl);
    if (host != QLatin1String(kPublicHost) && (eff.domain.isEmpty() || host != eff.domain))
        return {};
    QVariantMap result;
    result.insert(QStringLiteral("domain"), host);
    result.insert(QStringLiteral("roomName"), QUrl::fromPercentEncoding(room.toUtf8()));
    return result;
}

QString getJitsiMeetingUrl(const QString &content, const QString &meetUrl)
{
    // jitsi.ts getJitsiMeetingUrl: an exact effective-base prefix is the
    // whole trimmed body; otherwise scan for an http(s) URL on the effective
    // domain or meet.jit.si.
    const auto eff = effective(meetUrl);
    const auto trimmed = content.trimmed();
    if (!eff.baseUrl.isEmpty()) {
        if (trimmed.startsWith(eff.baseUrl + QLatin1Char('/')))
            return trimmed;
        if (trimmed == eff.baseUrl)
            return QString();
    }
    const auto domain = QRegularExpression::escape(eff.domain);
    const QRegularExpression pattern(QStringLiteral("https?://(%1|meet\\.jit\\.si)/[^\\s<>\"']+").arg(domain),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(trimmed);
    return match.hasMatch() ? match.captured(0) : QString();
}

QString createJitsiCallKey(const QString &ownerKey, const QString &meetingUrl)
{
    return ownerKey.trimmed() + QLatin1Char(':') + normalizedUrl(meetingUrl);
}

QString formatRoomDisplayName(const QString &meetingUrl, const QString &meetUrl)
{
    const auto room = parseJitsiUrl(meetingUrl, meetUrl).value(QStringLiteral("roomName")).toString().trimmed();
    if (room.isEmpty())
        return QString();
    auto display = room;
    display.replace(QRegularExpression(QStringLiteral("[-_]+")), QStringLiteral(" "));
    return display.trimmed();
}

} // namespace workspace
