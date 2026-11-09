#pragma once

#include <QUrl>
#include <optional>

namespace workspace {

// Shareable message links and the ew:// external protocol. The contracts and
// rejection semantics are pinned in docs/NATIVE_DESKTOP_MESSAGE_LINKS.md and
// mirror the Source deeplink-route.lib.ts / org-route.ts behavior.

// Accepted external deep link returns its internal route ("/..." with query
// and fragment). Anything outside the pinned contract returns std::nullopt.
std::optional<QString> parseExternalDeepLink(const QString &value);

struct MessageRoute {
    QString org;
    QString project;
    QString message;
};

// Extracts the org/project/message triple from an accepted "/message/" route.
std::optional<MessageRoute> parseMessageRoute(const QString &route);

// Source buildOrgRouteIdFromOrigin: deterministic organization route segment
// derived from a deployment origin.
QString orgRouteIdFromOrigin(const QUrl &origin);
QString orgRouteIdFromOrigin(const QString &origin);

// encodeURIComponent semantics for dynamic route segments.
QString encodeRouteSegment(const QString &value);

QString messageRoute(const QString &orgSegment, const QString &projectUuid,
                     const QString &messageUuid);

// Canonical shareable URL for one message on one server origin.
QString messageUrl(const QUrl &origin, const QString &projectUuid,
                   const QString &messageUuid);

} // namespace workspace
