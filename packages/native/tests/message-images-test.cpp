#include "message-images.h"
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
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

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
    MessageImages images{&api, &session, &files};
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
        session.login(server.origin(), "synthetic-message-viewer", "synthetic-password");
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

class MessageImagesTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }
    void pureReadsAndFullImageReferences();
    void firstFrameAspectAndNaturalSize_data();
    void firstFrameAspectAndNaturalSize();
    void invalidSvgAndPixelOverflow_data();
    void invalidSvgAndPixelOverflow();
    void entryLeasePipelineAndExactCancellation();
    void byteLruAndPinnedAdmissionRecover();
    void newMountAndExplicitRetryAreActions();
    void projectAndRetiredDecodeFence();
    void reentrantRetryAndWorkerTeardown_data();
    void reentrantRetryAndWorkerTeardown();
};

void MessageImagesTest::pureReadsAndFullImageReferences()
{
    Fixture f; auto tagged=bands(); tagged.setText("SourceOnly","not retained in decoded preview"); f.bytes=encoded(tagged);
    QCOMPARE(f.images.acquire(urn()),quint64(0)); QVERIFY(!f.images.retry(urn()));
    QVERIFY(f.start());
    for(const auto &value:QStringList{"", "urn:image:bad", "urn:file:"+fileId(), "urn:video:"+fileId(),
                                      "https://example.invalid/a.png", urn()+"/x"}) {
        QCOMPARE(f.images.acquire(value),quint64(0)); QVERIFY(!f.images.retry(value));
        QVERIFY(f.images.imageUrl(value).isEmpty()); QVERIFY(f.images.info(value).isEmpty()); QVERIFY(!f.images.failed(value));
    }
    for(int i=0;i<1000;++i) { QVERIFY(f.images.imageUrl(urn()).isEmpty()); QVERIFY(f.images.info(urn()).isEmpty()); QVERIFY(!f.images.failed(urn())); }
    QCOMPARE(f.metadataGets,0); QCOMPARE(f.binaryGets,0);
    const auto first=f.images.acquire(urn()+"?name=received.png&content_type=image%2Fpng&w=9999&h=1&size=0");
    const auto second=f.images.acquire("URN:IMAGE:"+fileId().toUpper()); QVERIFY(first); QVERIFY(second);
    QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());
    QCOMPARE(f.images.info(urn()),(QVariantMap{{"width",256},{"height",128}}));
    QCOMPARE(f.metadataGets,1); QCOMPARE(f.binaryGets,1); QCOMPARE(f.images.stats().leases,2);
    for(const auto &authorization:f.authorizations) QCOMPARE(authorization,QByteArray("Bearer synthetic-project-token"));
    std::unique_ptr<QQuickImageProvider> provider(f.images.createImageProvider());
    const auto preview=providerImage(*provider,f.images.imageUrl(urn()),{8000,8000});
    QCOMPARE(preview.size(),QSize(256,128)); QVERIFY(preview.textKeys().isEmpty());
    QVERIFY(mostly(preview.pixelColor(0,0),Qt::red));
    QVERIFY(mostly(preview.pixelColor(255,127),Qt::yellow)); // Full corners, no mask/crop/upscale.
    const auto url=f.images.imageUrl(urn()); f.images.release(first); f.images.release(second);
    const auto hit=f.images.acquire(urn()); QVERIFY(hit); QCOMPARE(f.images.imageUrl(urn()),url);
    QCOMPARE(f.metadataGets,1); QCOMPARE(f.binaryGets,1); f.images.release(hit);
    const auto empty=providerImage(*provider,"image://messageimages/retired/"+fileId());
    QCOMPARE(empty.size(),QSize(1,1)); QCOMPARE(empty.pixelColor(0,0).alpha(),0);
}

void MessageImagesTest::firstFrameAspectAndNaturalSize_data()
{
    QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<QSize>("natural"); QTest::addColumn<QSize>("preview"); QTest::addColumn<int>("colors");
    QTest::newRow("panorama")<<encoded(bands({4096,1024}),"png",0)<<QSize(4096,1024)<<QSize(1440,360)<<0;
    QTest::newRow("portrait")<<encoded(bands({100,4000}),"png",0)<<QSize(100,4000)<<QSize(9,360)<<0;
    QTest::newRow("jpeg-oriented-no-upscale")<<orientedJpeg()<<QSize(128,256)<<QSize(128,256)<<1;
    QTest::newRow("webp-complete32")<<QByteArray::fromBase64("UklGRhgAAABXRUJQVlA4TAwAAAAvB8AAAGhfe7rc/wA=")<<QSize(8,4)<<QSize(8,4)<<2;
    QTest::newRow("gif-first-static-frame")<<QByteArray::fromHex("47494638396101000100800000ff00000000ff21f90400000000002c000000000100010000020244010021f90400000000002c00000000010001000002024c01003b")<<QSize(1,1)<<QSize(1,1)<<3;
    QTest::newRow("extreme-wide")<<encoded(solid({65535,1},Qt::magenta),"png",0)<<QSize(65535,1)<<QSize(2048,1)<<4;
    QTest::newRow("exact16mp")<<encoded(solid({4000,4000},Qt::magenta),"png",0)<<QSize(4000,4000)<<QSize(360,360)<<4;
}
void MessageImagesTest::firstFrameAspectAndNaturalSize()
{
    QFETCH(QByteArray,bytes); QFETCH(QSize,natural); QFETCH(QSize,preview); QFETCH(int,colors);
    QVERIFY(!bytes.isEmpty()); QVERIFY(bytes.size()<=FileClient::MemoryDownloadLimitBytes);
    Fixture f; QVERIFY(f.start()); f.bytes=bytes; const auto lease=f.images.acquire(urn()); QVERIFY(lease);
    QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty()); QVERIFY(!f.images.failed(urn()));
    QCOMPARE(f.images.info(urn()),(QVariantMap{{"width",natural.width()},{"height",natural.height()}}));
    std::unique_ptr<QQuickImageProvider> provider(f.images.createImageProvider());
    const auto image=providerImage(*provider,f.images.imageUrl(urn()),{9000,9000}); QCOMPARE(image.size(),preview);
    QCOMPARE(image.pixelColor(0,0).alpha(),255); QCOMPARE(image.pixelColor(preview.width()-1,preview.height()-1).alpha(),255);
    if(colors==0) { QVERIFY(mostly(image.pixelColor(0,preview.height()/2),Qt::red)); QVERIFY(mostly(image.pixelColor(preview.width()-1,preview.height()/2),Qt::yellow)); }
    else if(colors==1) { QVERIFY(mostly(image.pixelColor(preview.width()/2,2),Qt::red)); QVERIFY(mostly(image.pixelColor(preview.width()/2,preview.height()-3),Qt::yellow)); }
    else if(colors==2) QVERIFY(mostly(image.pixelColor(3,2),QColor(61,125,203)));
    else if(colors==3) QVERIFY(mostly(image.pixelColor(0,0),Qt::red));
    else QVERIFY(mostly(image.pixelColor(preview.width()/2,preview.height()/2),Qt::magenta));
    QCOMPARE(f.images.stats().previewBytes,qint64(image.sizeInBytes())); QCOMPARE(f.metadataGets,1); QCOMPARE(f.binaryGets,1);
    f.images.release(lease);
}

void MessageImagesTest::invalidSvgAndPixelOverflow_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("invalid")<<QByteArray("not an image");
    QTest::newRow("svg-inert")<<QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\"><image href=\"https://example.invalid/private\"/></svg>");
    QTest::newRow("16mp-plus-valid")<<encoded(solid({4000,4001},Qt::magenta),"png",0);
}
void MessageImagesTest::invalidSvgAndPixelOverflow()
{
    QFETCH(QByteArray,bytes); QVERIFY(!bytes.isEmpty()); QVERIFY(bytes.size()<=FileClient::MemoryDownloadLimitBytes);
    Fixture f; QVERIFY(f.start()); f.bytes=bytes; const auto lease=f.images.acquire(urn()); QVERIFY(lease);
    QTRY_VERIFY(f.images.failed(urn())); QVERIFY(f.images.imageUrl(urn()).isEmpty()); QVERIFY(f.images.info(urn()).isEmpty());
    QCOMPARE(f.metadataGets,1); QCOMPARE(f.binaryGets,1); QCOMPARE(f.images.stats().previews,0);
    for(int i=0;i<1000;++i) { QVERIFY(f.images.failed(urn())); QVERIFY(f.images.info(urn()).isEmpty()); }
    QCOMPARE(f.metadataGets,1); f.images.release(lease);
}

void MessageImagesTest::entryLeasePipelineAndExactCancellation()
{
    Fixture f; QVERIFY(f.start()); f.holdMetadata=true; QList<quint64> leases;
    for(int i=1;i<=64;++i) { const auto lease=f.images.acquire(urn(i)); QVERIFY(lease); leases.append(lease); QVERIFY(f.images.stats().pipelineJobs<=2); }
    QCOMPARE(f.images.acquire(urn(65)),quint64(0)); QTRY_COMPARE(f.metadataGets,2);
    QCOMPARE(f.images.stats().entries,64); QCOMPARE(f.images.stats().queued,62);
    QPointer<QNetworkReply> cancelled;
    for(auto *reply:f.api.findChildren<QNetworkReply *>()) if(reply->url().path().endsWith(fileId())) cancelled=reply;
    QVERIFY(cancelled&&cancelled->isRunning()); f.images.release(leases.takeFirst());
    QTRY_VERIFY(!cancelled||!cancelled->isRunning()); QTRY_COMPARE(f.metadataGets,3);
    QVERIFY(f.requestedIds.contains(fileId(3))); QCOMPARE(f.images.stats().pipelineJobs,2);
    bool independentDone=false; FileResult independent;
    f.holdMetadata=false; f.files.downloadBytes(fileId(100),[&](const FileResult &result){independent=result;independentDone=true;});
    for(const auto lease:leases) f.images.release(lease);
    QTRY_VERIFY(independentDone); QVERIFY(independent.ok()); QCOMPARE(independent.bytes,f.bytes);
    QTRY_COMPARE(f.images.stats().pipelineJobs,0); QCOMPARE(f.images.stats().leases,0);
    for(const auto &release:f.releases)release(); QVERIFY(f.images.imageUrl(urn()).isEmpty());
    QList<quint64> same;
    for(int i=0;i<MessageImages::MaximumLeases;++i) {const auto lease=f.images.acquire(urn(200));QVERIFY(lease);same.append(lease);}
    QCOMPARE(f.images.acquire(urn(200)),quint64(0)); QTRY_VERIFY(!f.images.imageUrl(urn(200)).isEmpty());
    QCOMPARE(f.images.stats().leases,4096); for(const auto lease:same)f.images.release(lease);
}

void MessageImagesTest::byteLruAndPinnedAdmissionRecover()
{
    Fixture f; QVERIFY(f.start()); f.bytes=encoded(solid({2048,360},Qt::green),"png",0); QVERIFY(!f.bytes.isEmpty());
    QList<quint64> pinned;
    for(int i=1;i<=11;++i) { const auto lease=f.images.acquire(urn(i));QVERIFY(lease);pinned.append(lease);QTRY_VERIFY(!f.images.imageUrl(urn(i)).isEmpty()); }
    QCOMPARE(f.images.stats().previewBytes,qint64(11)*2048*360*4); QVERIFY(f.images.stats().previewBytes<=MessageImages::MaximumRetainedBytes);
    const auto blocked=f.images.acquire(urn(12));QVERIFY(blocked);QTRY_VERIFY(f.images.failed(urn(12)));
    QCOMPARE(f.images.stats().previews,11);QVERIFY(f.images.info(urn(12)).isEmpty());QCOMPARE(f.metadataGets,12);
    for(int i=0;i<1000;++i)QVERIFY(f.images.imageUrl(urn(12)).isEmpty());QCOMPARE(f.metadataGets,12);
    f.images.release(pinned.takeFirst());QVERIFY(f.images.retry(urn(12)));QVERIFY(!f.images.retry(urn(12)));
    QTRY_VERIFY(!f.images.imageUrl(urn(12)).isEmpty());QVERIFY(f.images.imageUrl(urn(1)).isEmpty());
    QCOMPARE(f.metadataGets,13);QCOMPARE(f.images.stats().previews,11);QVERIFY(f.images.stats().previewBytes<=MessageImages::MaximumRetainedBytes);
    QVERIFY(!f.images.retry(urn(12)));QVERIFY(!f.images.retry(urn(99)));
    for(const auto lease:pinned)f.images.release(lease);f.images.release(blocked);
    for(int i=13;i<=66;++i) {
        const auto lease=f.images.acquire(urn(i));QVERIFY(lease);QTRY_VERIFY(!f.images.imageUrl(urn(i)).isEmpty());f.images.release(lease);
        QVERIFY(f.images.stats().entries<=64);QVERIFY(f.images.stats().previewBytes<=MessageImages::MaximumRetainedBytes);
    }
    QVERIFY(f.images.imageUrl(urn(12)).isEmpty());QVERIFY(!f.images.imageUrl(urn(66)).isEmpty());
}

void MessageImagesTest::newMountAndExplicitRetryAreActions()
{
    Fixture f;QVERIFY(f.start());f.metadataStatus=403;
    const auto old=f.images.acquire(urn());QVERIFY(old);QTRY_VERIFY(f.images.failed(urn()));QCOMPARE(f.metadataGets,1);
    for(int i=0;i<1000;++i){QVERIFY(f.images.failed(urn()));QVERIFY(f.images.info(urn()).isEmpty());}
    f.metadataStatus=200;f.holdMetadata=true;
    const auto fresh=f.images.acquire(urn());QVERIFY(fresh);QTRY_COMPARE(f.metadataGets,2);
    const auto third=f.images.acquire(urn());QVERIFY(third);QCOMPARE(f.metadataGets,2);QCOMPARE(f.images.stats().pipelineJobs,1);QVERIFY(!f.images.retry(urn()));
    for(const auto &release:f.releases)release();f.holdMetadata=false;
    QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());QCOMPARE(f.binaryGets,1);
    f.images.release(old);f.images.release(fresh);f.images.release(third);
    f.bytes="invalid static image";const auto failed=f.images.acquire(urn(2));QVERIFY(failed);QTRY_VERIFY(f.images.failed(urn(2)));
    const auto requests=f.metadataGets;for(int i=0;i<1000;++i)QVERIFY(f.images.failed(urn(2)));QCOMPARE(f.metadataGets,requests);
    f.bytes=encoded(bands());QVERIFY(f.images.retry(urn(2)));QTRY_VERIFY(!f.images.imageUrl(urn(2)).isEmpty());QCOMPARE(f.metadataGets,requests+1);
    f.images.release(failed);
    f.bytes="invalid again";const auto another=f.images.acquire(urn(3));QVERIFY(another);QTRY_VERIFY(f.images.failed(urn(3)));
    f.bytes=encoded(bands());const auto newMount=f.images.acquire(urn(3));QVERIFY(newMount);QTRY_VERIFY(!f.images.imageUrl(urn(3)).isEmpty());
    f.images.release(another);f.images.release(newMount);QCOMPARE(f.images.stats().leases,0);
}

void MessageImagesTest::projectAndRetiredDecodeFence()
{
    bool retired=false,slotCounted=false;QList<quint64> next;
    Fixture f;f.offerProjectB=true;QVERIFY(f.start());
    const auto old=f.images.acquire(urn());QVERIFY(old);QTRY_VERIFY(!f.images.imageUrl(urn()).isEmpty());
    std::unique_ptr<QQuickImageProvider> provider(f.images.createImageProvider());const auto oldUrl=f.images.imageUrl(urn());
    const auto generation=f.api.generation(),scope=f.images.scopeRevision();f.holdProjectRefresh=true;f.session.selectProject(f.projectB);
    QTRY_COMPARE(f.heldRefreshPosts,1);QVERIFY(!f.session.ready());QCOMPARE(f.api.generation(),generation);
    QVERIFY(f.images.scopeRevision()>scope);QCOMPARE(providerImage(*provider,oldUrl).size(),QSize(1,1));QVERIFY(f.images.info(urn()).isEmpty());
    QVERIFY(!f.images.acquire(urn()));f.images.release(old);for(const auto &release:f.releases)release();f.holdProjectRefresh=false;
    // Fixture's generic introspection is for project A; use a fresh normal login
    // rather than fabricating a project-B identity or Session readiness.
    f.session.logout();QVERIFY(f.start());
    const auto connection=connect(&f.images,&MessageImages::revisionChanged,this,[&]{
        if(retired||!f.images.stats().decodeJobs)return;retired=true;
        f.api.setAccessToken("synthetic-message-new-generation");
        next.append(f.images.acquire(urn(2)));next.append(f.images.acquire(urn(3)));
        const auto state=f.images.stats();slotCounted=state.decodeJobs==1&&state.pipelineJobs==2&&state.queued==1;
    });
    const auto disconnectObserver=qScopeGuard([connection]{QObject::disconnect(connection);});
    QVERIFY(f.images.acquire(urn()));QTRY_VERIFY(retired);QVERIFY(slotCounted);
    QTRY_VERIFY(!f.images.imageUrl(urn(2)).isEmpty()&&!f.images.imageUrl(urn(3)).isEmpty());
    QVERIFY(f.images.imageUrl(urn()).isEmpty());QCOMPARE(f.images.stats().pipelineJobs,0);
    for(const auto lease:next)f.images.release(lease);
}

void MessageImagesTest::reentrantRetryAndWorkerTeardown_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("retry-new-scope")<<0;QTest::newRow("retry-destroy")<<1;QTest::newRow("worker-destroy")<<2;
}
void MessageImagesTest::reentrantRetryAndWorkerTeardown()
{
    QFETCH(int,mode);bool reentered=false;quint64 nested=0;
    Fixture f;QVERIFY(f.start());auto manager=std::make_unique<MessageImages>(&f.api,&f.session,&f.files);QPointer<MessageImages> alive(manager.get());
    std::unique_ptr<QQuickImageProvider> provider(manager->createImageProvider());
    f.metadataStatus=mode==2?200:403;const auto first=manager->acquire(urn());QVERIFY(first);
    if(mode!=2)QTRY_VERIFY(manager->failed(urn()));
    f.metadataStatus=200;f.holdMetadata=mode!=2;
    const auto connection=connect(manager.get(),&MessageImages::revisionChanged,this,[&]{
        if(reentered||!alive||(mode==2?!alive->stats().decodeJobs:alive->failed(urn())))return;
        reentered=true;if(mode){manager.reset();return;}
        f.api.setAccessToken("synthetic-message-nested-owner");if(alive)nested=alive->acquire(urn(2));
    });
    const auto disconnectObserver=qScopeGuard([connection]{QObject::disconnect(connection);});
    if(mode!=2)QCOMPARE(alive->acquire(urn()),quint64(0));
    QTRY_VERIFY(reentered);
    if(mode) {
        QVERIFY(!alive);const auto empty=providerImage(*provider,"image://messageimages/retired/"+fileId());
        QCOMPARE(empty.size(),QSize(1,1));QCOMPARE(empty.pixelColor(0,0).alpha(),0);return;
    }
    QVERIFY(alive);QVERIFY(nested);QTRY_COMPARE(f.requestedIds.count(fileId(2)),1);
    QCOMPARE(alive->stats().leases,1);alive->release(first);QCOMPARE(alive->stats().leases,1);
    for(const auto &release:f.releases)release();QTRY_VERIFY(!alive->imageUrl(urn(2)).isEmpty());
    QVERIFY(alive->imageUrl(urn()).isEmpty());alive->release(nested);
}

QTEST_GUILESS_MAIN(MessageImagesTest)
#include "message-images-test.moc"
