#pragma once

#include <QString>
#include <QVariantMap>

namespace workspace {

// Pure port of the incoming-invite builders from workspace_ui 61dacef7
// (packages/web/src/features/jitsi-call/workspace-jitsi-incoming-call.lib.ts
// and shared/lib/env.ts). See docs/NATIVE_DESKTOP_INCOMING_CALL_UI.md.

// Input keys: messageId, messageRead, messageOwn, authorUuid, payloadKind,
// content, streamKnown, streamName, streamPrivate, streamDirectUser,
// authorDisplayName, authorAvatarUrl, currentUserUuid,
// currentUserDisplayName, ownerKey, meetUrl, createdAtMs. Returns an empty
// map when the message must not raise an invite (reference null result).
QVariantMap buildIncomingDmCallInvite(const QVariantMap &input);

// Port of env.ts:132-135 (VITE_CALL_INCOMING_MODAL_VARIANT): lowercase,
// only "compact" selects the compact toast, anything else falls back to
// "large".
QString resolveIncomingCallVariant(const QString &raw);

} // namespace workspace
