#pragma once

#include <QByteArray>
#include <QtEndian>

namespace workspace {
// Qt 6.10's WebP handler peeks sizeof(WebPBitstreamFeatures) (40 bytes)
// before scanning. A complete legal lossless RIFF/WebP can be only 32 bytes.
// Pad only outside an already complete declared RIFF container for that peek;
// never manufacture missing bytes inside a truncated container. Transport length
// and checksum continue to validate the untouched original before decoding.
inline QByteArray nativeImageReaderInput(QByteArray bytes)
{
    constexpr qsizetype QtWebpFeaturePeekBytes = 40;
    if (bytes.size() < 12 || bytes.size() >= QtWebpFeaturePeekBytes
        || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WEBP") return bytes;
    const quint64 end = quint64(qFromLittleEndian<quint32>(bytes.constData() + 4)) + 8;
    if (end < 12 || end > quint64(bytes.size())) return bytes;
    // A matching outer RIFF length alone does not establish complete chunks.
    // Inspect the original short container, before adding any trailing bytes.
    for (quint64 position = 12; position < end;) {
        if (end - position < 8) return bytes;
        const quint64 length = qFromLittleEndian<quint32>(bytes.constData() + position + 4);
        const quint64 next = position + 8 + length + (length & 1);
        if (next > end) return bytes;
        position = next;
    }
    bytes.resize(QtWebpFeaturePeekBytes, char(0));
    return bytes;
}
} // namespace workspace
