// Installed Linux runtime regression: synthetic transient keys, no account I/O or network.
#include "native-chat-crypto.h"
#include "image-decode-fixtures.h"
#include "native-image-input.h"

#include <QBuffer>
#include <QImageReader>
#include <cstdlib>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSslSocket>
#include <openssl/ssl.h>
#include <cstdio>

using Crypto = workspace::NativeChatCrypto;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (OPENSSL_init_ssl(0, nullptr) != 1) return 10;
    const auto prefix = QFileInfo(QCoreApplication::applicationDirPath() + "/..").canonicalFilePath() + '/';
    const auto key = Crypto::createGroupKey();
    const auto pair = Crypto::createRequestKeyPair();
    if (!key || !pair) return 2;
    const Crypto::Context context{"10000000-0000-4000-8000-000000000001", "20000000-0000-4000-8000-000000000002"};
    const auto sealed = Crypto::encryptMessage(key.bytes, "synthetic stage probe", context);
    if (!sealed) return 3;
    const auto opened = Crypto::decryptMessage(key.bytes, sealed.bytes, context);
    if (!opened || opened.bytes != "synthetic stage probe") return 4;
    const Crypto::GrantContext grantContext{context.streamUuid, context.keyUuid,
        "30000000-0000-4000-8000-000000000003", "sender", "recipient"};
    const auto grant = Crypto::encryptGroupKeyGrant(key.bytes, pair.publicKey, grantContext);
    if (!grant) return 5;
    const auto granted = Crypto::decryptGroupKeyGrant(pair.privateKey, pair.publicKey, grant.bytes, grantContext);
    if (!granted || granted.bytes != key.bytes) return 6;
    QJsonObject imageRoundTrips;
    bool imagesDecoded = true;
    for (const auto &fixture : NativeDecodeFixtures) {
        auto bytes = workspace::nativeImageReaderInput(QByteArray::fromBase64(fixture.base64));
        QBuffer input(&bytes);
        input.open(QIODevice::ReadOnly);
        QImageReader reader(&input);
        reader.setAutoTransform(true);
        const auto image = reader.read();
        bool okay = !image.isNull() && image.size() == QSize(8, 4);
        if (okay) {
            const auto color = image.pixelColor(4, 2);
            // JPEG is lossy; the exact tiny golden fixture differs by <=2.
            const int tolerance = QByteArray(fixture.format) == "jpeg" ? 2 : 0;
            okay = std::abs(color.red() - 61) <= tolerance
                && std::abs(color.green() - 125) <= tolerance
                && std::abs(color.blue() - 203) <= tolerance && color.alpha() == 255;
        }
        imageRoundTrips.insert(QString::fromLatin1(fixture.format), okay);
        imagesDecoded = imagesDecoded && okay;
    }
    const bool tls = QSslSocket::supportsSsl();
    QFile maps("/proc/self/maps");
    if (!maps.open(QIODevice::ReadOnly)) return 7;
    bool allInside = true, cryptoMapped = false, sslMapped = false;
    QSet<QString> paths;
    for (const auto &line : maps.readAll().split('\n')) {
        const auto path = QString::fromUtf8(line.mid(line.lastIndexOf(' ') + 1));
        if (!path.contains("/libQt6") && !path.contains("/qt6/plugins/")
            && !path.contains("/libcrypto.so") && !path.contains("/libssl.so")
            && !path.contains("/libwebp") && !path.contains("/libpng")
            && !path.contains("/libjpeg") && !path.contains("/libsharpyuv") && !path.contains("/libmng") && !path.contains("/libtiff") && !path.contains("/liblcms2")) continue;
        allInside = allInside && QFileInfo(path).canonicalFilePath().startsWith(prefix);
        cryptoMapped = cryptoMapped || path.contains("/libcrypto.so");
        sslMapped = sslMapped || path.contains("/libssl.so");
        paths.insert(path);
    }
    auto sortedPaths = paths.values();
    sortedPaths.sort();
    QJsonArray runtimePaths;
    for (const auto &path : sortedPaths) runtimePaths.append(path);
    const QJsonObject output{{"imageRoundTrips", imageRoundTrips}, {"imagesDecoded", imagesDecoded},
        {"runtimePaths", runtimePaths}, {"messageRoundTrip", true}, {"grantRoundTrip", true},
        {"tlsSupported", tls}, {"tlsBackend", QSslSocket::activeBackend()}, {"tlsVersion", QSslSocket::sslLibraryVersionString()},
        {"runtimePathsInsidePrefix", allInside}, {"cryptoMapped", cryptoMapped}, {"sslMapped", sslMapped},
        {"relevantLibraries", paths.size()}};
    const auto json = QJsonDocument(output).toJson(QJsonDocument::Compact);
    if (std::fwrite(json.constData(), 1, size_t(json.size()), stdout) != size_t(json.size())) return 9;
    return tls && allInside && cryptoMapped && sslMapped && imagesDecoded ? 0 : 8;
}
