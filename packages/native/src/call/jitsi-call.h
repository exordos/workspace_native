#pragma once

#include <QString>
#include <QVariantMap>

namespace workspace {

// Native port of the reference Jitsi URL contract
// (workspace_ui 61dacef7 packages/web/src/shared/lib/jitsi.ts:30-65,77-106 and
// features/jitsi-call/workspace-jitsi-call.lib.ts:27-45). See
// docs/NATIVE_DESKTOP_CALL_LAUNCH.md. The build-time default host is the
// native counterpart of VITE_JITSI_MEET_DOMAIN and only affects URL
// recognition; it never enables the header call entry point.
inline constexpr auto kDefaultJitsiMeetBase = "https://meet.jit.si";

// Reference normalizeRoomSegment: trim, every run of non-[a-z0-9-] becomes
// '-', collapse repeats, strip edge '-', lowercase; empty becomes "room".
QString normalizeRoomSegment(const QString &value);

// workspace-{org}-{project}-{streamUuid}-{topicUuid}-{nowMs}, every segment
// normalized, exactly six segments joined with '-'.
QString buildWorkspaceJitsiRoomName(const QString &organizationId, const QString &projectId,
    const QString &streamUuid, const QString &topicUuid, qint64 nowMs);

// server_settings.meet_url -> origin only (lowercase scheme/host, port kept);
// http/https only, anything else returns an empty string.
QString normalizeMeetOrigin(const QString &value);

// Effective base URL: normalized server-settings value first, otherwise the
// build-time default. Empty only when both are unusable.
QString effectiveMeetBase(const QString &meetUrl);

// ${base}/${encodeURIComponent(roomName)}.
QString buildJitsiMeetingUrl(const QString &roomName, const QString &meetUrl = {});

// {domain, roomName} when the URL is http(s) on the effective host or
// meet.jit.si, otherwise an empty map. roomName is percent-decoded.
QVariantMap parseJitsiUrl(const QString &url, const QString &meetUrl = {});

// First Jitsi meeting URL inside free text (configured host or meet.jit.si).
QString getJitsiMeetingUrl(const QString &content, const QString &meetUrl = {});

// ownerKey + ':' + normalized meeting URL (trimmed, trailing slashes cut,
// lowercase scheme/host; an unparseable URL falls back to the trimmed string).
QString createJitsiCallKey(const QString &ownerKey, const QString &meetingUrl);

// Room display name: '-'/'_' runs -> spaces; empty when there is no room.
QString formatRoomDisplayName(const QString &meetingUrl, const QString &meetUrl = {});

} // namespace workspace
