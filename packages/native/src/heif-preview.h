#pragma once

#include <QByteArray>
#include <QImage>
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

// Decode HEIC/HEIF bytes to a packed RGBA8888 preview image. Backed by the
// system libheif (HEVC via the libde265 plugin); returns false with a
// human-readable reason on any failure so callers fall back to the file card
// exactly like the reference display-error path. Bytes above kHeifMaxFileBytes
// are rejected before the library sees them.
bool heifDecodeRgba(const QByteArray &bytes, QImage &out, QString &error);

} // namespace workspace
