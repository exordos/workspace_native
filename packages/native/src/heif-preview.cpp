#include "heif-preview.h"

#include <QRegularExpression>

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

} // namespace workspace
