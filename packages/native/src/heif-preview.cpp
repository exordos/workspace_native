#include "heif-preview.h"

#include <QRegularExpression>

#ifdef WORKSPACE_NATIVE_WITH_LIBHEIF
#include <libheif/heif.h>
#include <memory>
#include <mutex>
#endif

namespace workspace {

namespace {

// Brand/MIME tables cloned verbatim from the reference
// shared/lib/heic-preview-format.lib.ts at e0194ff.
const QSet<QByteArray> &heicMimeTypes()
{
    static const QSet<QByteArray> values {
        "image/heic", "image/heif", "image/heic-sequence", "image/heif-sequence" };
    return values;
}

const QSet<QByteArray> &heicBrands()
{
    static const QSet<QByteArray> values {
        "heic", "heix", "heim", "heis", "hevc", "hevx", "hevm", "hevs" };
    return values;
}

const QSet<QByteArray> &genericHeifBrands()
{
    static const QSet<QByteArray> values { "mif1", "msf1" };
    return values;
}

const QSet<QByteArray> &otherImageBrands()
{
    static const QSet<QByteArray> values { "avif", "avis", "jpeg" };
    return values;
}

quint32 readBe32(const QByteArray &header, int offset)
{
    return (quint32(quint8(header.at(offset))) << 24)
        | (quint32(quint8(header.at(offset + 1))) << 16)
        | (quint32(quint8(header.at(offset + 2))) << 8)
        | quint32(quint8(header.at(offset + 3)));
}

quint64 readBe64(const QByteArray &header, int offset)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | quint8(header.at(offset + i));
    return value;
}

QByteArray readBrand(const QByteArray &header, int offset)
{
    return header.mid(offset, 4);
}

} // namespace

bool heifLooksLikeHeic(const QString &fileName, const QString &contentType)
{
    // Reference strips at the first ';', trims and lowercases before the set
    // lookup; an empty or absent MIME falls through to the name regex.
    QString mimeType = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (!mimeType.isEmpty() && heicMimeTypes().contains(mimeType.toUtf8()))
        return true;
    if (!mimeType.isEmpty() && mimeType != QLatin1String("application/octet-stream")
        && mimeType != QLatin1String("binary/octet-stream")) {
        return false;
    }
    static const QRegularExpression heicName(QStringLiteral("\\.hei[cf]$"),
        QRegularExpression::CaseInsensitiveOption);
    return heicName.match(fileName.trimmed()).hasMatch();
}

bool heifSniffFtyp(const QByteArray &headerBytes)
{
    const QByteArray header = headerBytes.left(kHeifFtypHeaderLimit);
    if (header.size() < 16 || readBrand(header, 4) != QByteArray("ftyp"))
        return false;

    const quint32 shortSize = readBe32(header, 0);
    const int brandOffset = shortSize == 1 ? 16 : 8;
    if (header.size() < brandOffset + 8)
        return false;
    const quint64 boxSize = shortSize == 1 ? readBe64(header, 8) : quint64(shortSize);
    if (boxSize < quint64(brandOffset + 8) || boxSize > quint64(header.size())
        || boxSize % 4 != 0) {
        return false;
    }

    const QByteArray majorBrand = readBrand(header, brandOffset);
    if (otherImageBrands().contains(majorBrand))
        return false;
    // The major brand, then every compatible brand inside the declared box.
    if (heicBrands().contains(majorBrand))
        return true;
    bool genericCompatible = genericHeifBrands().contains(majorBrand);
    for (int offset = brandOffset + 8; offset < int(boxSize); offset += 4) {
        const QByteArray brand = readBrand(header, offset);
        if (heicBrands().contains(brand))
            return true;
        if (otherImageBrands().contains(brand))
            genericCompatible = false;
    }
    return genericCompatible;
}

#ifdef WORKSPACE_NATIVE_WITH_LIBHEIF
namespace {

// Ubuntu builds libheif with the codec plugins in a separate directory that
// the library does NOT auto-register: without heif_load_plugins the HEVC
// decoder stays invisible and every preview decode fails. Run once per
// process over every directory libheif itself reports (built-in path plus
// LIBHEIF_PLUGIN_PATHS, which the installed runtime uses for the bundled
// plugin tree).
void ensureHeifPluginsLoaded()
{
    static std::once_flag loadedOnce;
    std::call_once(loadedOnce, [] {
        if (const char *const *dirs = heif_get_plugin_directories()) {
            for (int i = 0; dirs[i] != nullptr; ++i) {
                const heif_plugin_info *infos[16] = {};
                int count = 0;
                heif_load_plugins(dirs[i], infos, &count, 16);
            }
        }
    });
}

} // namespace

bool heifDecodeRgba(const QByteArray &bytes, QImage &out, QString &error, qint64 maxSourcePixels)
{
    out = QImage();
    if (bytes.isEmpty()) {
        error = QStringLiteral("heic preview: empty payload");
        return false;
    }
    if (bytes.size() > kHeifMaxFileBytes) {
        error = QStringLiteral("heic preview: file above 25 MiB cap");
        return false;
    }
    ensureHeifPluginsLoaded();
    heif_context *context = heif_context_alloc();
    if (!context) {
        error = QStringLiteral("heic preview: libheif context allocation failed");
        return false;
    }
    auto contextGuard = std::unique_ptr<heif_context, void (*)(heif_context *)>(
        context, [](heif_context *c) { heif_context_free(c); });
    if (heif_error read = heif_context_read_from_memory(context, bytes.constData(),
            bytes.size(), nullptr); read.code != heif_error_Ok) {
        error = QStringLiteral("heic preview: %1").arg(QString::fromLatin1(read.message));
        return false;
    }
    heif_image_handle *handle = nullptr;
    if (heif_error found = heif_context_get_primary_image_handle(context, &handle);
        found.code != heif_error_Ok) {
        error = QStringLiteral("heic preview: %1").arg(QString::fromLatin1(found.message));
        return false;
    }
    auto handleGuard = std::unique_ptr<heif_image_handle, void (*)(const heif_image_handle *)>(
        handle, [](const heif_image_handle *h) { heif_image_handle_release(h); });
    // Pixel budget checked on the handle BEFORE decoding: a container may
    // claim huge dimensions inside a small file, and materialising that
    // canvas first would defeat the budget (16 MP matches the shared
    // preview cap; the reference caps the file size only, recorded deviation).
    const qint64 width = heif_image_handle_get_width(handle);
    const qint64 height = heif_image_handle_get_height(handle);
    if (maxSourcePixels > 0 && width > 0 && height > 0 && width * height > maxSourcePixels) {
        error = QStringLiteral("heic preview: %1x%2 above pixel budget").arg(width).arg(height);
        return false;
    }
    heif_image *image = nullptr;
    if (heif_error decoded = heif_decode_image(handle, &image, heif_colorspace_RGB,
            heif_chroma_interleaved_RGBA, nullptr); decoded.code != heif_error_Ok) {
        error = QStringLiteral("heic preview: %1").arg(QString::fromLatin1(decoded.message));
        return false;
    }
    auto imageGuard = std::unique_ptr<heif_image, void (*)(const heif_image *)>(
        image, [](const heif_image *i) { heif_image_release(i); });
    int stride = 0;
    const uint8_t *pixels = heif_image_get_plane(image, heif_channel_interleaved, &stride);
    if (!pixels) {
        error = QStringLiteral("heic preview: decoder returned no RGBA plane");
        return false;
    }
    out = QImage(pixels, heif_image_handle_get_width(handle),
        heif_image_handle_get_height(handle), stride, QImage::Format_RGBA8888).copy();
    if (out.isNull()) {
        error = QStringLiteral("heic preview: image copy failed");
        return false;
    }
    error = QString();
    return true;
}
#else
bool heifDecodeRgba(const QByteArray &, QImage &out, QString &error, qint64)
{
    // Only reachable on non-Linux builds without libheif provisioned
    // (desktop-platform-packages open item); Linux configure makes libheif
    // a hard requirement, so shipped binaries always decode.
    out = QImage();
    error = QStringLiteral("heic preview: libheif not available in this build");
    return false;
}
#endif

} // namespace workspace
