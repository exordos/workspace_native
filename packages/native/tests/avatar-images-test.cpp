#include "avatar-images.h"
#include "api-client.h"
#include "file-client.h"
#include "session.h"
#include "mock-server.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <atomic>
#include <thread>

using namespace workspace;
using namespace workspace::test;

namespace {
QString fileId(int index = 1)
{
    return QString("aaaaaaaa-aaaa-4aaa-8aaa-%1").arg(index, 12, 16, QLatin1Char('0'));
}
QString urn(int index = 1) { return "urn:image:" + fileId(index); }
QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
QByteArray json(const QJsonValue &value)
{
    return value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                           : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
}
QImage bands(const QSize &size = {256, 128})
{
    QImage image(size, QImage::Format_RGB32);
    const QColor colors[]{Qt::red, Qt::green, Qt::blue, Qt::yellow};
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) image.setPixelColor(x, y, colors[qMin(3, x * 4 / image.width())]);
    return image;
}
QImage solid(const QSize &size, const QColor &color)
{
    QImage image(size, QImage::Format_RGB32); image.fill(color); return image;
}
QByteArray encoded(const QImage &image, const QByteArray &format = "png", int quality = 100)
{
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format); writer.setQuality(quality);
    return writer.write(image) ? bytes : QByteArray{};
}
QString providerKey(const QString &url) { return QUrl(url).path().mid(1); }
QImage providerImage(QQuickImageProvider &provider, const QString &url, QSize requested = {})
{
    QSize size; return provider.requestImage(providerKey(url), &size, requested);
}
bool mostly(const QColor &color, const QColor &expected)
{
    return qAbs(color.red() - expected.red()) < 20 && qAbs(color.green() - expected.green()) < 20
        && qAbs(color.blue() - expected.blue()) < 20 && color.alpha() == 255;
}

// Same one-origin raw HTTP arrangement as file-client-test. JSON auth replies
// use the existing sessionResponse fixture; image bytes remain binary. Deferred
// replies hold only immutable bytes + QPointer socket, not a second server.
class ImageServer final : public QTcpServer {
public:
    std::function<void(QTcpSocket *, const Request &)> handler;
    ImageServer()
    {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    if (socket->property("handled").toBool()) return;
                    *buffer += socket->readAll();
                    const auto end = buffer->indexOf("\r\n\r\n"); if (end < 0) return;
                    const auto lines = buffer->left(end).split('\n'); const auto first = lines.first().trimmed().split(' ');
                    if (first.size() < 2) return;
                    Request request; request.method = first.at(0); request.target = QUrl::fromEncoded(first.at(1));
                    for (int i = 1; i < lines.size(); ++i) {
                        const auto colon = lines[i].indexOf(':');
                        if (colon > 0) request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
                    }
                    const auto length = request.headers.value("content-length").toInt();
                    if (buffer->size() < end + 4 + length) return;
                    request.body = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
                    socket->setProperty("handled", true);
                    if (handler) handler(socket, request);
                });
            }
        });
    }
    ~ImageServer() override { for (auto *socket : findChildren<QTcpSocket *>()) socket->abort(); }
    QUrl origin() const { return QUrl(QString("http://127.0.0.1:%1").arg(serverPort())); }
    static void reply(QTcpSocket *socket, const QByteArray &bytes, int status = 200)
    {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    }
};

struct Fixture {
    ImageServer server;
    QByteArray bytes = encoded(bands());
    QString viewer = userId;
    const QString projectB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    bool offerProjectB = false, holdProjectRefresh = false; int heldRefreshPosts = 0;
    bool holdMetadata = false;
    int metadataStatus = 200, metadataGets = 0, binaryGets = 0;
    QList<QPointer<QTcpSocket>> held;
    QList<std::function<void()>> releases;
    QStringList requestedIds;
    QList<QByteArray> authorizations;
    ApiClient api{nullptr, true};
    Session session{&api};
    FileClient files{&api};
    AvatarImages images{&api, &session, &files};
    Fixture()
    {
        server.handler = [this](QTcpSocket *socket, const Request &request) {
            const auto path = request.target.path();
            const QString prefix = "/api/workspace/v1/messenger/files/";
            if (!path.startsWith(prefix)) {
                auto result = sessionResponse(request);
                if (offerProjectB && path == "/api/core/v1/iam/projects/")
                    result.data = QJsonArray{QJsonObject{{"uuid", projectId}, {"name", "Project A"}, {"status", "ACTIVE"}},
                        QJsonObject{{"uuid", projectB}, {"name", "Project B"}, {"status", "ACTIVE"}}};
                if (holdProjectRefresh && path.endsWith("/actions/get_token/invoke")
                    && request.body.value("grant_type") == "refresh_token") {
                    ++heldRefreshPosts; held.append(socket);
                    releases.append([guard = QPointer<QTcpSocket>(socket), body = json(result.data), status = result.status] {
                        if (guard) ImageServer::reply(guard, body, status);
                    }); return;
                }
                if (path.endsWith("/actions/introspect")) {
                    auto body = result.data.toObject(); body["user_info"] = QJsonObject{{"uuid", viewer}}; result.data = body;
                }
                ImageServer::reply(socket, json(result.data), result.status); return;
            }
            authorizations.append(request.headers.value("authorization"));
            if (path.endsWith("/actions/download")) { ++binaryGets; ImageServer::reply(socket, bytes); return; }
            ++metadataGets; const auto uuid = path.mid(prefix.size()); requestedIds.append(uuid);
            const auto body = json(QJsonObject{{"uuid", uuid}, {"stream_uuid", QJsonValue::Null}, {"name", "avatar"},
                {"content_type", "image/test"}, {"size_bytes", bytes.size()}, {"hash", QString::fromLatin1(digest(bytes))}});
            if (holdMetadata) {
                held.append(socket);
                releases.append([guard = QPointer<QTcpSocket>(socket), body, status = metadataStatus] {
                    if (guard) ImageServer::reply(guard, body, status);
                });
            } else ImageServer::reply(socket, body, metadataStatus);
        };
    }
    bool start()
    {
        if (!server.isListening() || bytes.isEmpty()) return false;
        session.login(server.origin(), "synthetic-avatar-viewer", "synthetic-password");
        if (!QTest::qWaitFor([this] { return !session.projects().isEmpty(); })) return false;
        session.selectProject(projectId);
        return QTest::qWaitFor([this] { return session.ready(); });
    }
};

QByteArray orientedJpeg()
{
    auto bytes = encoded(bands(), "jpeg");
    // Little-endian EXIF IFD: orientation=6 (90 degrees clockwise).
    const auto exif = QByteArray::fromHex("45786966000049492a0008000000010012010300010000000600000000000000");
    QByteArray segment("\xff\xe1", 2); const auto length = exif.size() + 2;
    segment.append(char(length >> 8)); segment.append(char(length)); segment += exif;
    bytes.insert(2, segment); return bytes;
}
} // namespace

class AvatarImagesTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }
    void pureReadsInvalidAndNoOwnerNeverFetch();
    void realDownloadDeduplicatesAndMasks();
    void pipelineAndAdmissionAreBoundedAndCancelExactTransfer();
    void repeatedConsumersHaveBoundedSmallLeases();
    void retainedThumbnailsAreBounded();
    void decodeFailuresStayFallbackUntilScopeChanges();
    void decoderFormatsAndCrop_data();
    void decoderFormatsAndCrop();
    void rejectedHeadersAndFormats_data();
    void rejectedHeadersAndFormats();
    void ownerAndApiGenerationFenceNetworkAndDecode();
    void retiredDecodeKeepsItsPipelineSlot();
    void notificationMayDestroyManagerDuringDecode();
    void providerOutlivesManagerAndNeverFetches();
    void transportFailureRetriesOnlyAfterLastRelease();
    void projectSelectionInvalidatesBeforeHeldTokenReply();
};

void AvatarImagesTest::pureReadsInvalidAndNoOwnerNeverFetch()
{
    Fixture f;
    QCOMPARE(f.images.acquire(urn()), quint64(0));
    QVERIFY(f.start());
    for (const auto &value : QStringList{"", "urn:IMAGE:" + fileId(), "urn:image:bad", "https://example.invalid/avatar", urn() + "/x", QString(QChar(0x85)) + urn()}) {
        QCOMPARE(f.images.acquire(value), quint64(0)); QVERIFY(f.images.imageUrl(value).isEmpty()); QVERIFY(!f.images.failed(value));
    }
    for (int i = 0; i < 1000; ++i) { QVERIFY(f.images.imageUrl(urn()).isEmpty()); QVERIFY(!f.images.failed(urn())); }
    std::unique_ptr<QQuickImageProvider> provider(f.images.createImageProvider());
    const auto placeholder = providerImage(*provider, "image://avatars/retired/" + fileId());
    QCOMPARE(placeholder.size(), QSize(1, 1)); QCOMPARE(placeholder.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(f.metadataGets, 0); QCOMPARE(f.binaryGets, 0); QCOMPARE(f.images.stats().leases, 0);
}

void AvatarImagesTest::realDownloadDeduplicatesAndMasks()
{
    Fixture f; QVERIFY(f.start());
    const auto first = f.images.acquire(urn()); const auto second = f.images.acquire(QString(QChar(0xfeff)) + urn().toUpper().replace("URN:IMAGE:", "urn:image:") + QChar(0xfeff));
    QVERIFY(first); QVERIFY(second); QVERIFY(first != second);
    QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());
    QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 1); QCOMPARE(f.images.stats().pipelineJobs, 0);
    QCOMPARE(f.images.stats().leases, 2); QVERIFY(!f.images.failed(urn()));
    for (const auto &authorization : f.authorizations) QCOMPARE(authorization, QByteArray("Bearer synthetic-project-token"));
    std::unique_ptr<QQuickImageProvider> provider(f.images.createImageProvider());
    const auto url = f.images.imageUrl(urn());
    const auto image = providerImage(*provider, url, {4096, 4096});
    QCOMPARE(image.size(), QSize(128, 128)); QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    QVERIFY(mostly(image.pixelColor(20, 64), Qt::green)); QVERIFY(mostly(image.pixelColor(108, 64), Qt::blue));
    f.images.release(first); QCOMPARE(f.images.imageUrl(urn()), url); f.images.release(second);
    const auto cached = f.images.acquire(urn()); QVERIFY(cached); QCOMPARE(f.images.imageUrl(urn()), url);
    QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 1); f.images.release(cached);
}

void AvatarImagesTest::pipelineAndAdmissionAreBoundedAndCancelExactTransfer()
{
    Fixture f; QVERIFY(f.start()); f.holdMetadata = true;
    QList<quint64> leases;
    for (int i = 1; i <= 64; ++i) { const auto lease = f.images.acquire(urn(i)); QVERIFY(lease); leases.append(lease); QVERIFY(f.images.stats().pipelineJobs <= 2); }
    QCOMPARE(f.images.acquire(urn(65)), quint64(0));
    QTRY_COMPARE(f.metadataGets, 2); QCOMPARE(f.binaryGets, 0);
    QCOMPARE(f.images.stats().entries, 64); QCOMPARE(f.images.stats().leases, 64); QCOMPARE(f.images.stats().queued, 62);
    QPointer<QNetworkReply> cancelled;
    for (auto *reply : f.api.findChildren<QNetworkReply *>()) if (reply->url().path().endsWith(fileId())) cancelled = reply;
    QVERIFY(cancelled && cancelled->isRunning());
    f.images.release(leases.takeFirst());
    QTRY_VERIFY(!cancelled || !cancelled->isRunning()); QTRY_COMPARE(f.metadataGets, 3);
    QVERIFY(f.requestedIds.contains(fileId(3))); QCOMPARE(f.images.stats().pipelineJobs, 2);
    // Another owner's operation on the shared FileClient must survive release.
    bool independentDone = false; FileResult independent;
    f.holdMetadata = false;
    f.files.downloadBytes(fileId(100), [&](const FileResult &result) { independent = result; independentDone = true; });
    for (const auto lease : leases) { f.images.release(lease); QVERIFY(f.images.stats().pipelineJobs <= 2); }
    QTRY_VERIFY(independentDone); QVERIFY(independent.ok()); QCOMPARE(independent.bytes, f.bytes);
    QTRY_COMPARE(f.images.stats().pipelineJobs, 0); QCOMPARE(f.images.stats().leases, 0);
    // Late release of cancelled exact sockets is safe and cannot publish images.
    for (const auto &release : f.releases) release();
    QVERIFY(f.images.imageUrl(urn()).isEmpty()); QCOMPARE(f.images.stats().thumbnails, 0);
}

void AvatarImagesTest::repeatedConsumersHaveBoundedSmallLeases()
{
    Fixture f; QVERIFY(f.start()); QList<quint64> leases;
    for (int i = 0; i < AvatarImages::MaximumLeases; ++i) {
        const auto lease = f.images.acquire(urn()); QVERIFY(lease); leases.append(lease);
    }
    QCOMPARE(f.images.acquire(urn()), quint64(0));
    QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());
    QCOMPARE(f.images.stats().leases, 4096); QCOMPARE(f.images.stats().entries, 1);
    QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 1); QCOMPARE(f.images.stats().thumbnails, 1);
    for (const auto lease : leases) f.images.release(lease);
    QCOMPARE(f.images.stats().leases, 0); QCOMPARE(f.images.stats().thumbnails, 1);
}

void AvatarImagesTest::retainedThumbnailsAreBounded()
{
    Fixture f; QVERIFY(f.start());
    for (int i = 1; i <= 65; ++i) {
        const auto lease = f.images.acquire(urn(i)); QVERIFY(lease);
        QTRY_VERIFY(!f.images.imageUrl(urn(i)).isEmpty()); f.images.release(lease);
        QVERIFY(f.images.stats().entries <= 64); QVERIFY(f.images.stats().thumbnails <= 64);
        QVERIFY(f.images.stats().thumbnailBytes <= qint64(64) * 128 * 128 * 4);
    }
    QCOMPARE(f.metadataGets, 65); QCOMPARE(f.binaryGets, 65); QCOMPARE(f.images.stats().thumbnails, 64);
    QVERIFY(f.images.imageUrl(urn(1)).isEmpty());
    const auto lease = f.images.acquire(urn(1)); QVERIFY(lease); QTRY_VERIFY(!f.images.imageUrl(urn(1)).isEmpty());
    QCOMPARE(f.metadataGets, 66); QCOMPARE(f.binaryGets, 66); f.images.release(lease);
}

void AvatarImagesTest::decodeFailuresStayFallbackUntilScopeChanges()
{
    Fixture f; QVERIFY(f.start()); f.bytes = "not an image";
    auto lease = f.images.acquire(urn()); QVERIFY(lease); QTRY_VERIFY(f.images.failed(urn()));
    QVERIFY(f.images.imageUrl(urn()).isEmpty()); QCOMPARE(f.binaryGets, 1); f.images.release(lease);
    f.bytes = encoded(bands()); lease = f.images.acquire(urn()); QVERIFY(lease);
    for (int i = 0; i < 1000; ++i) { QVERIFY(f.images.failed(urn())); QVERIFY(f.images.imageUrl(urn()).isEmpty()); }
    // Positive unrelated completion allows queued work to progress, without retrying failure.
    const auto other = f.images.acquire(urn(2)); QVERIFY(other); QTRY_VERIFY(!f.images.imageUrl(urn(2)).isEmpty());
    QCOMPARE(f.metadataGets, 2); f.images.release(other); f.images.release(lease);
    const auto scope = f.images.scopeRevision(); f.api.setAccessToken("synthetic-same-owner-refresh");
    QVERIFY(f.images.scopeRevision() > scope); QVERIFY(!f.images.failed(urn())); QCOMPARE(f.images.stats().entries, 0);
    lease = f.images.acquire(urn()); QVERIFY(lease); QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());
    QCOMPARE(f.metadataGets, 3); f.images.release(lease);
}

void AvatarImagesTest::decoderFormatsAndCrop_data()
{
    QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<int>("orientation");
    QTest::newRow("png") << encoded(bands()) << 0;
    QTest::newRow("jpeg") << encoded(bands(), "jpeg") << 0;
    QTest::newRow("jpeg-exif-clockwise") << orientedJpeg() << 1;
    // Exact locally generated decoder-only fixture from Root's installed-image probe.
    QTest::newRow("webp-real-plugin-required") << QByteArray::fromBase64("UklGRhgAAABXRUJQVlA4TAwAAAAvB8AAAGhfe7rc/wA=") << 4;
    QTest::newRow("webp-short-complete-with-trailing-bytes") << (QByteArray::fromBase64("UklGRhgAAABXRUJQVlA4TAwAAAAvB8AAAGhfe7rc/wA=") + QByteArray(2,char(0))) << 4;
    // Real 2-frame GIF: first red, second blue. Only first static frame is admitted.
    QTest::newRow("gif-first-frame-only") << QByteArray::fromHex("47494638396101000100800000ff00000000ff21f90400000000002c000000000100010000020244010021f90400000000002c00000000010001000002024c01003b") << 2;
    QTest::newRow("very-wide-positive") << encoded(solid({65535, 1}, Qt::magenta)) << 3;
    QTest::newRow("very-tall-positive") << encoded(solid({1, 65535}, Qt::magenta)) << 3;
    QTest::newRow("exact-16mp-positive") << encoded(solid({4000, 4000}, Qt::magenta), "png", 0) << 3;
}
void AvatarImagesTest::decoderFormatsAndCrop()
{
    QFETCH(QByteArray, bytes); QFETCH(int, orientation);
    // Missing decoder/encoder is an environment/setup FAIL, never an invalid-user-image PASS.
    QVERIFY2(!bytes.isEmpty(), "Required static image format encoder/plugin unavailable");
    QVERIFY2(bytes.size() <= FileClient::MemoryDownloadLimitBytes, "Decoder fixture must reach the decoder within the actual transfer cap");
    QBuffer probe; probe.setData(bytes); QVERIFY(probe.open(QIODevice::ReadOnly)); QImageReader reader(&probe);
    QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
    if (orientation == 2) { QVERIFY(reader.supportsAnimation()); QCOMPARE(reader.imageCount(), 2); }
    if (orientation == 4) QCOMPARE(reader.format().toLower(), QByteArray("webp"));
    Fixture f; QVERIFY(f.start()); f.bytes = bytes; const auto lease = f.images.acquire(urn()); QVERIFY(lease);
    QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty()); QVERIFY(!f.images.failed(urn()));
    std::unique_ptr<QQuickImageProvider> provider(f.images.createImageProvider()); const auto image = providerImage(*provider, f.images.imageUrl(urn()));
    QCOMPARE(image.size(), QSize(128, 128)); QCOMPARE(image.pixelColor(0, 0).alpha(), 0); QCOMPARE(image.pixelColor(64, 64).alpha(), 255);
    if (orientation == 0) { QVERIFY(mostly(image.pixelColor(20, 64), Qt::green)); QVERIFY(mostly(image.pixelColor(108, 64), Qt::blue)); }
    if (orientation == 1) { QVERIFY(mostly(image.pixelColor(64, 20), Qt::green)); QVERIFY(mostly(image.pixelColor(64, 108), Qt::blue)); }
    if (orientation == 2) QVERIFY(mostly(image.pixelColor(64, 64), Qt::red));
    if (orientation == 3) QVERIFY(mostly(image.pixelColor(64, 64), Qt::magenta));
    if (orientation == 4) QVERIFY(mostly(image.pixelColor(64, 64), QColor(61, 125, 203)));
    QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 1); f.images.release(lease);
}

void AvatarImagesTest::rejectedHeadersAndFormats_data()
{
    QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<bool>("overPixels");
    // Valid encoded image, not merely a patched/truncated header: without the
    // pre-decode size guard it would decode and incorrectly become Ready.
    QTest::newRow("16mp-plus-valid-header") << encoded(solid({4000, 4001}, Qt::magenta), "png", 0) << true;
    QTest::newRow("svg-never-rendered") << QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\"><image href=\"https://example.invalid/private\"/></svg>") << false;
    QTest::newRow("webp-truncated-declared-container") << QByteArray::fromBase64("UklGRhgAAABXRUJQVlA4TAwAAAAvB8AAAGhfe7rc/wA=").chopped(1) << false;
    auto adjusted = QByteArray::fromBase64("UklGRhgAAABXRUJQVlA4TAwAAAAvB8AAAGhfe7rc/wA=").chopped(1);
    adjusted[4] = char(adjusted.size() - 8); // Outer RIFF now fits, original inner VP8L still extends past it.
    QTest::newRow("webp-truncated-chunk-adjusted-riff") << adjusted << false;
    QTest::newRow("invalid-bytes") << QByteArray("not an image") << false;
}
void AvatarImagesTest::rejectedHeadersAndFormats()
{
    QFETCH(QByteArray, bytes); QFETCH(bool, overPixels); QVERIFY(!bytes.isEmpty());
    QVERIFY2(bytes.size() <= FileClient::MemoryDownloadLimitBytes, "Reject-by-decoder fixture must reach the decoder within the actual transfer cap");
    if (overPixels) {
        QBuffer probe; probe.setData(bytes); QVERIFY(probe.open(QIODevice::ReadOnly)); QImageReader reader(&probe);
        QCOMPARE(reader.format().toLower(), QByteArray("png"));
        QVERIFY(qint64(reader.size().width()) * reader.size().height() > AvatarImages::MaximumSourcePixels);
    }
    Fixture f; QVERIFY(f.start()); f.bytes = bytes; const auto lease = f.images.acquire(urn()); QVERIFY(lease);
    QTRY_VERIFY(f.images.failed(urn())); QVERIFY(f.images.imageUrl(urn()).isEmpty());
    QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 1); QCOMPARE(f.images.stats().thumbnails, 0); f.images.release(lease);
}

void AvatarImagesTest::ownerAndApiGenerationFenceNetworkAndDecode()
{
    // These flags outlive Fixture and its signals, even if an assertion returns early.
    bool changedDuringDecode = false; int stalePublications = 0;
    Fixture f; QVERIFY(f.start()); f.holdMetadata = true;
    auto lease = f.images.acquire(urn()); QVERIFY(lease); QTRY_COMPARE(f.metadataGets, 1);
    const auto oldScope = f.images.scopeRevision();
    QSignalSpy scopeChanged(&f.images, &AvatarImages::scopeChanged); QVERIFY(scopeChanged.isValid());
    f.api.setAccessToken("synthetic-refreshed-token");
    QVERIFY(f.images.scopeRevision() > oldScope); QCOMPARE(scopeChanged.size(), 1);
    QCOMPARE(f.images.stats().leases, 0); QCOMPARE(f.images.stats().entries, 0); QVERIFY(!f.images.failed(urn()));
    const auto oldGets = f.metadataGets; for (const auto &release : f.releases) release();
    QVERIFY(f.images.imageUrl(urn()).isEmpty()); QCOMPARE(f.metadataGets, oldGets);
    f.holdMetadata = false;
    const auto connection = connect(&f.images, &AvatarImages::revisionChanged, this, [&] {
        if (f.images.stats().decodeJobs && !changedDuringDecode) {
            changedDuringDecode = true; f.api.setAccessToken("synthetic-refresh-during-queued-decode");
        }
        if (changedDuringDecode && !f.images.imageUrl(urn()).isEmpty()) ++stalePublications;
    });
    const auto disconnectObserver = qScopeGuard([connection] { QObject::disconnect(connection); });
    lease = f.images.acquire(urn()); QVERIFY(lease); QTRY_VERIFY(changedDuringDecode);
    QTRY_COMPARE(f.images.stats().pipelineJobs, 0); QCOMPARE(stalePublications, 0); QVERIFY(f.images.imageUrl(urn()).isEmpty());
    QCOMPARE(f.images.stats().entries, 0); QCOMPARE(f.images.stats().leases, 0);
    f.session.logout(); QVERIFY(!f.session.ready()); QCOMPARE(f.images.acquire(urn()), quint64(0));
    f.viewer = "99999999-9999-4999-8999-999999999999"; QVERIFY(f.start()); QCOMPARE(f.session.user(), f.viewer);
    lease = f.images.acquire(urn()); QVERIFY(lease); QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());
    QCOMPARE(stalePublications, 1); // Exactly this new-owner publication is now expected.
    f.images.release(lease);
}

void AvatarImagesTest::retiredDecodeKeepsItsPipelineSlot()
{
    bool boundaryObserved = false, retiredSlotCounted = false; QList<quint64> newLeases;
    Fixture f; QVERIFY(f.start());
    const auto connection = connect(&f.images, &AvatarImages::revisionChanged, this, [&] {
        if (boundaryObserved || !f.images.stats().decodeJobs) return;
        boundaryObserved = true;
        f.api.setAccessToken("synthetic-refresh-with-retired-decode");
        newLeases.append(f.images.acquire(urn(2))); newLeases.append(f.images.acquire(urn(3)));
        const auto state = f.images.stats();
        retiredSlotCounted = state.decodeJobs == 1 && state.pipelineJobs == 2 && state.queued == 1;
    });
    const auto disconnectObserver = qScopeGuard([connection] { QObject::disconnect(connection); });
    QVERIFY(f.images.acquire(urn())); QTRY_VERIFY(boundaryObserved); QVERIFY(retiredSlotCounted);
    QCOMPARE(newLeases.size(), 2); QVERIFY(newLeases[0]); QVERIFY(newLeases[1]);
    QTRY_VERIFY(!f.images.imageUrl(urn(2)).isEmpty() && !f.images.imageUrl(urn(3)).isEmpty());
    QVERIFY(f.images.imageUrl(urn()).isEmpty()); QCOMPARE(f.images.stats().pipelineJobs, 0);
    QCOMPARE(f.images.stats().thumbnails, 2); QCOMPARE(f.metadataGets, 3); QCOMPARE(f.binaryGets, 3);
    for (const auto lease : newLeases) f.images.release(lease);
}

void AvatarImagesTest::notificationMayDestroyManagerDuringDecode()
{
    bool decodeObserved = false;
    Fixture f; QVERIFY(f.start());
    auto manager = std::make_unique<AvatarImages>(&f.api, &f.session, &f.files);
    QPointer<AvatarImages> alive(manager.get());
    std::unique_ptr<QQuickImageProvider> provider(manager->createImageProvider());
    const auto connection = connect(manager.get(), &AvatarImages::revisionChanged, this, [&] {
        if (!manager || !manager->stats().decodeJobs) return;
        decodeObserved = true; manager.reset(); // Natural bounded worker joins, no forceCompletion.
    });
    const auto disconnectObserver = qScopeGuard([connection] { QObject::disconnect(connection); });
    QVERIFY(manager->acquire(urn())); QTRY_VERIFY(decodeObserved); QVERIFY(!alive);
    const auto placeholder = providerImage(*provider, "image://avatars/retired/" + fileId());
    QCOMPARE(placeholder.size(), QSize(1, 1)); QCOMPARE(placeholder.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 1);
}

void AvatarImagesTest::providerOutlivesManagerAndNeverFetches()
{
    Fixture f; QVERIFY(f.start());
    auto images = std::make_unique<AvatarImages>(&f.api, &f.session, &f.files);
    std::unique_ptr<QQuickImageProvider> provider(images->createImageProvider());
    const auto lease = images->acquire(urn()); QVERIFY(lease); QTRY_VERIFY(!images->imageUrl(urn()).isEmpty());
    const auto url = images->imageUrl(urn()); const auto requests = f.metadataGets + f.binaryGets;
    std::atomic<bool> running{true}, badSize{false};
    std::thread reader([&] {
        while (running.load(std::memory_order_relaxed)) {
            const auto image = providerImage(*provider, url);
            if (image.size() != QSize(128, 128) && image.size() != QSize(1, 1)) badSize.store(true);
        }
    });
    const auto joinReader = qScopeGuard([&] { running.store(false); if (reader.joinable()) reader.join(); });
    images.reset();
    const auto retired = providerImage(*provider, url); QCOMPARE(retired.size(), QSize(1, 1)); QCOMPARE(retired.pixelColor(0, 0).alpha(), 0);
    running.store(false); reader.join();
    QCOMPARE(f.metadataGets + f.binaryGets, requests); QVERIFY(!badSize.load());
}

void AvatarImagesTest::projectSelectionInvalidatesBeforeHeldTokenReply()
{
    Fixture f; f.offerProjectB = true; QVERIFY(f.start());
    QCOMPARE(f.session.project(), projectId); QCOMPARE(f.session.projects().size(), 2);
    const auto lease = f.images.acquire(urn()); QVERIFY(lease); QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());
    std::unique_ptr<QQuickImageProvider> provider(f.images.createImageProvider()); const auto oldUrl = f.images.imageUrl(urn());
    QCOMPARE(providerImage(*provider, oldUrl).size(), QSize(128, 128));
    const auto generation = f.api.generation(), scope = f.images.scopeRevision();
    QSignalSpy scopeChanged(&f.images, &AvatarImages::scopeChanged); QVERIFY(scopeChanged.isValid());
    f.holdProjectRefresh = true; f.session.selectProject(f.projectB);
    QTRY_COMPARE(f.heldRefreshPosts, 1); // Actual admitted POST acknowledged, its response is still held.
    QVERIFY(!f.session.ready()); QCOMPARE(f.session.project(), f.projectB); QCOMPARE(f.api.generation(), generation);
    QVERIFY(f.images.imageUrl(urn()).isEmpty()); // Pure owner guard already fences this getter on RED.
    // Intended RED is the retained actual photo (128x128), before the token changes API generation.
    const auto retired = providerImage(*provider, oldUrl);
    QCOMPARE(retired.size(), QSize(1, 1)); QCOMPARE(retired.pixelColor(0, 0).alpha(), 0);
    QVERIFY(f.images.scopeRevision() > scope); QCOMPARE(scopeChanged.size(), 1);
    QCOMPARE(f.images.stats().entries, 0); QCOMPARE(f.images.stats().leases, 0);
    QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 1);
    // Deliberately never release token POST; fixture teardown cancels the real pending request.
}

void AvatarImagesTest::transportFailureRetriesOnlyAfterLastRelease()
{
    Fixture f; QVERIFY(f.start()); const auto scope = f.images.scopeRevision(); f.metadataStatus = 403;
    const auto first = f.images.acquire(urn()), second = f.images.acquire(urn()); QVERIFY(first); QVERIFY(second);
    QTRY_VERIFY(f.images.failed(urn())); QCOMPARE(f.metadataGets, 1); QCOMPARE(f.binaryGets, 0);
    f.metadataStatus = 200; f.images.release(first);
    for (int i = 0; i < 1000; ++i) { QVERIFY(f.images.failed(urn())); QVERIFY(f.images.imageUrl(urn()).isEmpty()); }
    const auto stillMounted = f.images.acquire(urn()); QVERIFY(stillMounted); f.images.release(stillMounted);
    QCOMPARE(f.metadataGets, 1); // Existing mounted consumer/revision reads cannot retry.
    f.images.release(second);
    auto freshMount = f.images.acquire(urn()); QVERIFY(freshMount);
    QTRY_COMPARE(f.metadataGets, 2); // Intended RED: retained failed entry leaves actual count at1.
    QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty()); QVERIFY(!f.images.failed(urn()));
    QCOMPARE(f.images.scopeRevision(), scope); QCOMPARE(f.binaryGets, 1); f.images.release(freshMount);
    // A cancelled last consumer must not leave a terminal failure for its next mount.
    f.holdMetadata = true; const auto cancelled = f.images.acquire(urn(2)); QVERIFY(cancelled);
    QTRY_COMPARE(f.metadataGets, 3); f.images.release(cancelled); QTRY_COMPARE(f.images.stats().pipelineJobs, 0);
    QVERIFY(!f.images.failed(urn(2))); f.holdMetadata = false;
    freshMount = f.images.acquire(urn(2)); QVERIFY(freshMount); QTRY_VERIFY(!f.images.imageUrl(urn(2)).isEmpty());
    QCOMPARE(f.metadataGets, 4); QCOMPARE(f.binaryGets, 2); f.images.release(freshMount);
    for (const auto &release : f.releases) release(); // Late cancelled response cannot replace the new thumbnail.
    QVERIFY(!f.images.imageUrl(urn(2)).isEmpty());
}

QTEST_GUILESS_MAIN(AvatarImagesTest)
#include "avatar-images-test.moc"
