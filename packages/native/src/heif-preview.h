#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace workspace {

// Caps cloned verbatim from the reference heic-preview.lib.ts at e0194ff.
inline constexpr qint64 kHeifMaxFileBytes = qint64(25) * 1024 * 1024;
inline constexpr int kHeifDecodeTimeoutMs = 15000;
inline constexpr int kHeifFtypHeaderLimit = 4096;
// Decode pixel budget. Sized to admit ordinary high-resolution HEIC photos
// (#670 finding 3): a 48 MP iPhone Pro frame is 8064x6048 = 48.8 MP, so the
// cap is 50 MP with margin. libheif 1.21 exposes no scaled-decode path for
// libde265 (heif_scaling_options is "not defined yet" in its public header),
// so the budget bounds the memory instead of the decode: the worst-case
// RGBA8888 canvas is 50 MP * 4 B = 200 MB. The budget still refuses a
// container claiming a bigger canvas BEFORE anything is materialised.
inline constexpr qint64 kHeifMaxDecodePixels = qint64(50) * 1000 * 1000;

// The budget gate applied to the handle dimensions before decoding (the
// exact predicate heifDecodeRgba uses; exposed for the contract tests).
inline bool heifPixelBudgetRejects(qint64 width, qint64 height, qint64 budgetPixels, QString &error)
{
    if (budgetPixels > 0 && width > 0 && height > 0 && width * height > budgetPixels) {
        error = QStringLiteral("heic preview: %1x%2 above pixel budget").arg(width).arg(height);
        return true;
    }
    return false;
}

// Metadata predicate (reference isHeicAttachment): MIME fast path first,
// filename regex only when the MIME is absent or a binary placeholder.
bool heifLooksLikeHeic(const QString &fileName, const QString &contentType);

// Content sniff (reference hasHeicFileType) over the first ISO-BMFF bytes.
bool heifSniffFtyp(const QByteArray &header);

// Cooperative cancellation hook (#680 fix round). When isCancelled is set,
// heifDecodeRgba consults it once BEFORE entering the library (a deterministic
// stop for jobs canceled while queued) and additionally hands it to libheif's
// heif_decoding_options::cancel_decoding (the v6 API), which libheif calls
// with progress_user_data at decode-stage checkpoints, so a cancel that
// arrives while the decoder is running also stops the work inside the
// library. The hook may be called from library background threads and must be
// thread-safe; `user` is passed through uninterpreted.
struct HeifCancelHook {
    bool (*isCancelled)(void *user) = nullptr;
    void *user = nullptr;
};

// Decode HEIC/HEIF bytes to a packed RGBA8888 preview image. Backed by the
// system libheif (HEVC via the libde265 plugin); returns false with a
// human-readable reason on any failure so callers fall back to the file card
// exactly like the reference display-error path. Bytes above kHeifMaxFileBytes
// are rejected before the library sees them, and an image above
// kHeifMaxDecodePixels is rejected before the canvas is materialised. A
// canceled decode fails with an error naming "cancelled".
bool heifDecodeRgba(const QByteArray &bytes, QImage &out, QString &error,
    qint64 maxSourcePixels = kHeifMaxDecodePixels, const HeifCancelHook &cancel = {});

} // namespace workspace
