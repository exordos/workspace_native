#pragma once

#include <QByteArray>
#include <QString>

namespace workspace {

// Caps cloned verbatim from the reference heic-preview.lib.ts at e0194ff.
inline constexpr qint64 kHeifMaxFileBytes = qint64(25) * 1024 * 1024;
inline constexpr int kHeifDecodeTimeoutMs = 15000;
inline constexpr int kHeifFtypHeaderLimit = 4096;

// Metadata predicate (reference isHeicAttachment): MIME fast path first,
// filename regex only when the MIME is absent or a binary placeholder.
bool heifLooksLikeHeic(const QString &fileName, const QString &contentType);

// Content sniff (reference hasHeicFileType) over the first ISO-BMFF bytes.
bool heifSniffFtyp(const QByteArray &header);

} // namespace workspace
