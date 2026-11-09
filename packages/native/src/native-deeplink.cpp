#include "native-deeplink.h"

#include <QRegularExpression>

namespace workspace {

namespace {

QUrl parseUrl(const QString &value)
{
    const QUrl url(value, QUrl::TolerantMode);
    return url.isValid() ? url : QUrl();
}

// Source isSafeDeeplinkRoute: bounds, script-like prefixes and encoded hosts.
bool isSafeDeeplinkRoute(const QString &route)
{
    const auto trimmed = route.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 512) return false;
    const auto lower = trimmed.toLower();
    if (lower.startsWith("javascript:") || lower.startsWith("data:") || lower.startsWith("vbscript:"))
        return false;
    return !trimmed.contains("//");
}

QString sanitizeOrgSegment(const QString &raw)
{
    QString normalized = raw.toLower();
    normalized.replace(QRegularExpression("[^a-z0-9.-]"), "-");
    normalized.replace(QRegularExpression("-+"), "-");
    while (normalized.startsWith('-')) normalized.remove(0, 1);
    while (normalized.endsWith('-')) normalized.chop(1);
    return normalized.isEmpty() ? QStringLiteral("org") : normalized;
}

const QRegularExpression &projectRoutePattern()
{
    static const QRegularExpression pattern(
        "^/org/[^/]+/project/[^/]+/(?:messenger|inbox|feed|activity/[^/]+|message/[^/]+"
        "|stream/[^/]+(?:/topic/[^/]+)?)/?(?:[?#].*)?$");
    return pattern;
}

} // namespace

std::optional<QString> parseExternalDeepLink(const QString &value)
{
    // Source contract: no backslashes and no characters at or below the space.
    for (const auto character : value)
        if (character == '\\' || character.unicode() <= 0x20) return std::nullopt;
    const QUrl url = parseUrl(value);
    if (!url.isValid()) return std::nullopt;
    if (url.scheme().compare("ew", Qt::CaseInsensitive) != 0) return std::nullopt;
    if (url.host(QUrl::FullyEncoded).compare("open", Qt::CaseInsensitive) != 0) return std::nullopt;
    if (url.port() != -1 || !url.userName().isEmpty() || !url.password().isEmpty())
        return std::nullopt;
    QString route = url.path(QUrl::FullyEncoded);
    if (route.isEmpty()) route = "/";
    if (url.hasQuery()) route += "?" + url.query(QUrl::FullyEncoded);
    if (url.hasFragment()) route += "#" + url.fragment(QUrl::FullyEncoded);
    if (!route.startsWith('/') || !isSafeDeeplinkRoute(route)) return std::nullopt;
    if (QRegularExpression("%(?:2f|5c|00|0a|0d)", QRegularExpression::CaseInsensitiveOption)
            .match(route).hasMatch())
        return std::nullopt;
    static const QRegularExpression uuid("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression messageSegment("/message/([^/?#]+)");
    const auto message = messageSegment.match(url.path(QUrl::FullyEncoded));
    if (message.hasMatch() && !uuid.match(message.captured(1)).hasMatch())
        return std::nullopt;
    if (route == "/") return route;
    if (QRegularExpression("^/org/[^/?#]+/?$").match(route).hasMatch()) return route;
    if (QRegularExpression("^/(?:calendar|mail|calls|licenses|settings|logs)/?(?:[?#].*)?$")
            .match(route).hasMatch())
        return route;
    if (QRegularExpression("^/org/[^/]+/(?:calendar|mail|calls|licenses|settings|logs)/?(?:[?#].*)?$")
            .match(route).hasMatch())
        return route;
    if (projectRoutePattern().match(route).hasMatch()) return route;
    return std::nullopt;
}

std::optional<MessageRoute> parseMessageRoute(const QString &route)
{
    static const QRegularExpression pattern(
        "^/org/([^/?#]+)/project/([^/?#]+)/message/([^/?#]+)/?(?:[?#].*)?$");
    const auto match = pattern.match(route);
    if (!match.hasMatch()) return std::nullopt;
    return MessageRoute{match.captured(1), match.captured(2), match.captured(3)};
}

QString orgRouteIdFromOrigin(const QUrl &origin)
{
    if (!origin.isValid() || origin.host().isEmpty()) return sanitizeOrgSegment(origin.toString());
    QString host = origin.host().toLower();
    if (origin.port() != -1) {
        const bool defaultPort = (origin.scheme() == "http" && origin.port() == 80)
            || (origin.scheme() == "https" && origin.port() == 443);
        if (!defaultPort) host += "-" + QString::number(origin.port());
    }
    return sanitizeOrgSegment(host);
}

QString orgRouteIdFromOrigin(const QString &origin)
{
    const auto trimmed = origin.trimmed();
    if (trimmed.isEmpty()) return QStringLiteral("org");
    const auto url = parseUrl(trimmed.contains("://") ? trimmed : "https://" + trimmed);
    if (!url.isValid() || url.host().isEmpty()) return sanitizeOrgSegment(trimmed);
    return orgRouteIdFromOrigin(url);
}

QString encodeRouteSegment(const QString &value)
{
    static const QRegularExpression unreserved("[A-Za-z0-9._~-]");
    QString encoded;
    const auto bytes = value.toUtf8();
    for (const auto raw : bytes) {
        const QString character = QString::fromLatin1(&raw, 1);
        if (unreserved.match(character).hasMatch()) encoded += character;
        else encoded += '%' + QString::number(static_cast<unsigned char>(raw), 16).toUpper().rightJustified(2, '0');
    }
    return encoded;
}

QString messageRoute(const QString &orgSegment, const QString &projectUuid,
                     const QString &messageUuid)
{
    return "/org/" + encodeRouteSegment(orgSegment) + "/project/" + encodeRouteSegment(projectUuid)
        + "/message/" + encodeRouteSegment(messageUuid);
}

QString messageUrl(const QUrl &origin, const QString &projectUuid, const QString &messageUuid)
{
    auto base = origin.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    while (base.endsWith('/')) base.chop(1);
    return base + messageRoute(orgRouteIdFromOrigin(origin), projectUuid, messageUuid);
}

} // namespace workspace
