#pragma once

#include "native-test-harness.h"

// Media viewer file slots used by the gallery tests (issue #8).
inline const QString mvFileA{"aaaaaaaa-0000-4000-8000-00000000aa01"};
inline const QString mvFileB{"bbbbbbbb-0000-4000-8000-00000000bb02"};
inline const QString mvFileC{"cccccccc-0000-4000-8000-00000000cc03"};

inline QVariantMap mediaViewerRow(const QString &id, const QString &fileUuid,
    const QString &kind = QStringLiteral("markdown"))
{
    const auto body = QStringLiteral("Head\n\n![%1.png](urn:image:%2?name=%1.png&content_type=image%2Fpng&size=3)\n\nTail")
        .arg(id, fileUuid);
    return QVariantMap{{"id", id}, {"kind", kind}, {"content", body}, {"displayContent", body}};
}

// A markdown row carrying one standalone video reference (issue #203,
// workspace-media-viewer-item.lib.ts:22-27 accepts mediaKind video).
inline QVariantMap mediaViewerVideoRow(const QString &id, const QString &fileUuid,
    const QString &kind = QStringLiteral("markdown"))
{
    const auto body = QStringLiteral("Head\n\n![%1.mp4](urn:video:%2?name=%1.mp4&content_type=video%2Fmp4&size=3)\n\nTail")
        .arg(id, fileUuid);
    return QVariantMap{{"id", id}, {"kind", kind}, {"content", body}, {"displayContent", body}};
}
