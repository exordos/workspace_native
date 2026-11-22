#include "chat-background.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace workspace;

// Issue #637 (docs/NATIVE_DESKTOP_CHAT_BACKGROUNDS.md section 5): restore and
// apply contract of the storage core plus the reference prepareChatBackground
// image pipeline rows (workspace_ui chat-background.lib.ts:81-114).
class NativeChatBackgroundTest final : public QObject {
    Q_OBJECT
private:
    static QByteArray rasterFixture(int width, int height, const QColor &color)
    {
        QImage image(width, height, QImage::Format_ARGB32);
        image.fill(color);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG"); // magic bytes are PNG regardless of name
        return bytes;
    }
    static QByteArray corruptJpeg()
    {
        QByteArray junk(16, '\0');
        junk[0] = char(0xff);
        junk[1] = char(0xd8);
        junk[2] = char(0xff);
        return junk;
    }
    static QImage decodeDataUrl(const QString &dataUrl)
    {
        const auto payload = dataUrl.mid(23); // len("data:image/jpeg;base64,")
        QImage image;
        image.loadFromData(QByteArray::fromBase64(payload.toLatin1()), "JPEG");
        return image;
    }

private slots:
    void parsePreferencesContract()
    {
        const auto empty = ChatBackground::parsePreferences(QString{});
        QVERIFY(empty.value("global").isNull());
        QVERIFY(empty.value("chats").toObject().isEmpty());
        // Malformed JSON and an over-cap raw both collapse to empty.
        QCOMPARE(ChatBackground::parsePreferences(QStringLiteral("{not json")).value("global"),
            QJsonValue::Null);
        QCOMPARE(ChatBackground::parsePreferences(QString(QByteArray(int(ChatBackground::kMaxRawLength) + 1, 'x')))
                     .value("global"),
            QJsonValue::Null);
        // Legacy bare value (no global/chats wrapper) is honoured up to the stored cap.
        const auto legacy = ChatBackground::parsePreferences(
            QStringLiteral("{\"preset\":\"sea\"}"));
        QCOMPARE(legacy.value("global").toObject().value("preset").toString(),
            QStringLiteral("sea"));
        // Wrapped payload: bad chat keys drop, explicit null defaults survive,
        // an image over the stored-image cap drops, unknown presets drop.
        const QString longImage = QStringLiteral("data:image/jpeg;base64,")
            + QString(int(ChatBackground::kMaxImageLength) - 23 + 1, QChar('A'));
        const QString scene = QStringLiteral(
                "{\"global\":{\"preset\":\"mountains\"},\"chats\":{"
                "\"stream:2f1a\":{\"preset\":\"sea\"},\"topic:2f1a\":null,"
                "\"stream:ZZ\":{\"preset\":\"forest\"},\"stream:1\":{\"preset\":\"nope\"},"
                "\"stream:2\":{\"image\":\"%1\"}}}")
            .arg(longImage);
        const auto parsed = ChatBackground::parsePreferences(scene);
        QCOMPARE(parsed.value("global").toObject().value("preset").toString(),
            QStringLiteral("mountains"));
        const auto chats = parsed.value("chats").toObject();
        QCOMPARE(chats.value(QStringLiteral("stream:2f1a")).toObject().value("preset").toString(),
            QStringLiteral("sea"));
        QVERIFY(chats.value(QStringLiteral("topic:2f1a")).isNull());
        QVERIFY(!chats.contains(QStringLiteral("stream:ZZ")));
        QVERIFY(!chats.contains(QStringLiteral("stream:1")));
        QVERIFY(!chats.contains(QStringLiteral("stream:2")));
    }

    void perOwnerStorageAndApplyScopes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings ini(dir.filePath("backgrounds.ini"), QSettings::IniFormat);
        ini.setFallbacksEnabled(false);
        ChatBackground backgrounds(&ini);
        QVERIFY(!backgrounds.available());
        QVERIFY(!backgrounds.applyAll(ChatBackground::Value{})); // no owner -> no writes
        backgrounds.setOwner(QStringLiteral("owner-a"));
        QVERIFY(backgrounds.available());
        const ChatBackground::Value sea{ChatBackground::Kind::Preset, QStringLiteral("sea")};
        const ChatBackground::Value mountains{
            ChatBackground::Kind::Preset, QStringLiteral("mountains")};
        QVERIFY(backgrounds.applyAll(mountains)); // global for owner-a
        QCOMPARE(backgrounds.resolve(QStringLiteral("stream:2f1a")).text,
            QStringLiteral("mountains"));
        QVERIFY(backgrounds.applyChat(QStringLiteral("stream:2f1a"), sea));
        QVERIFY(!backgrounds.applyChat(QStringLiteral("stream:ZZ"), sea)); // malformed key
        // A chat override, then an explicit per-chat default, both beat global.
        QCOMPARE(backgrounds.resolve(QStringLiteral("stream:2f1a")).text, QStringLiteral("sea"));
        QVERIFY(backgrounds.applyChat(QStringLiteral("topic:9c"), ChatBackground::Value{}));
        QVERIFY(backgrounds.resolve(QStringLiteral("topic:9c")).kind
            == ChatBackground::Kind::Default);
        QVERIFY(backgrounds.applyAll(mountains));
        // Reference scope "all" writes { global: value, chats: {} } (hook.ts:64),
        // so every chat override drops - explicit defaults included.
        QCOMPARE(backgrounds.resolve(QStringLiteral("topic:9c")).text,
            QStringLiteral("mountains"));
        QCOMPARE(backgrounds.resolve(QStringLiteral("stream:2f1a")).text,
            QStringLiteral("mountains"));
        // A second owner starts clean.
        backgrounds.setOwner(QStringLiteral("owner-b"));
        QVERIFY(backgrounds.resolve(QStringLiteral("stream:2f1a")).kind
            == ChatBackground::Kind::Default);
        // A fresh store view over the same file reloads owner-a's persisted state.
        ChatBackground reopened(&ini);
        reopened.setOwner(QStringLiteral("owner-a"));
        QCOMPARE(reopened.resolve(QStringLiteral("anything")).text,
            QStringLiteral("mountains"));
        QCOMPARE(reopened.resolve(QStringLiteral("stream:2f1a")).text,
            QStringLiteral("mountains"));
    }

    void prepareImageRejectsTypeMismatchAndBounds()
    {
        struct Row { QByteArray payload; QString declared; QString error; };
        const QByteArray png = rasterFixture(4, 4, Qt::red);
        QByteArray jpegBytes;
        {
            QImage image(8, 8, QImage::Format_ARGB32);
            image.fill(Qt::blue);
            QBuffer buffer(&jpegBytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "JPEG");
        }
        QByteArray gif(16, '\0');
        gif[0] = 'G'; gif[1] = 'I'; gif[2] = 'F'; gif[3] = '8';
        QByteArray riff(16, '\0');
        riff[0] = 'R'; riff[1] = 'I'; riff[2] = 'F'; riff[3] = 'F'; // no 'W' at byte 8
        QByteArray tooSmallWebp = riff; tooSmallWebp[8] = 'W';
        const QList<Row> rows{
            {QByteArray(), QStringLiteral("image/jpeg"), QStringLiteral("size")},
            {png, QStringLiteral("image/svg+xml"), QStringLiteral("type")},
            {gif, QStringLiteral("image/gif"), QStringLiteral("type")},
            {png, QStringLiteral("image/jpeg"), QStringLiteral("type")},
            {jpegBytes, QStringLiteral("image/png"), QStringLiteral("type")},
            {riff, QStringLiteral("image/webp"), QStringLiteral("type")},
            {tooSmallWebp, QStringLiteral("image/webp"), QStringLiteral("decode")},
            {QByteArray(8 * 1024 * 1024 + 1, '\0'), QStringLiteral("image/jpeg"),
                QStringLiteral("size")},
            {corruptJpeg(), QStringLiteral("image/jpeg"),
                QStringLiteral("decode")},
        };
        QStringList failures;
        for (const auto &row : rows) {
            const auto result = ChatBackground::prepareImage(row.payload, row.declared);
            if (row.error == QStringLiteral("decode") && result.error == QStringLiteral("type"))
                continue; // qt without a webp/suffix plugin may reject earlier; both reject
            if (result.ok || result.error != row.error)
                failures << QStringLiteral("%1 declared %2 got %3")
                                .arg(QString::fromLatin1(row.payload.left(4).toHex()), row.declared,
                                    result.error.isEmpty() ? QStringLiteral("ok") : result.error);
        }
        QCOMPARE(failures, QStringList{});
    }

    void prepareImageDownscalesToJpegOnWhite()
    {
        // 3200x1600 PNG lands on a 1600x800 JPEG (scale = 1600/3200).
        const auto large = ChatBackground::prepareImage(rasterFixture(3200, 1600, QColor(200, 30, 30)),
            QStringLiteral("image/png"));
        QVERIFY(large.ok);
        QVERIFY(large.dataUrl.startsWith(QStringLiteral("data:image/jpeg;base64,")));
        const QImage decoded = decodeDataUrl(large.dataUrl);
        QCOMPARE(decoded.width(), 1600);
        QCOMPARE(decoded.height(), 800);
        // A small image is never upscaled.
        const auto small = ChatBackground::prepareImage(rasterFixture(100, 50, QColor(10, 120, 10)),
            QStringLiteral("image/png"));
        QVERIFY(small.ok);
        const QImage kept = decodeDataUrl(small.dataUrl);
        QCOMPARE(kept.width(), 100);
        QCOMPARE(kept.height(), 50);
        // Alpha flattens onto white fill: a fully transparent pixel turns white.
        QImage transparent(16, 16, QImage::Format_ARGB32);
        transparent.fill(QColor(0, 0, 0, 0));
        QByteArray bytes;
        {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            transparent.save(&buffer, "PNG");
        }
        const auto flattened = ChatBackground::prepareImage(bytes, QStringLiteral("image/png"));
        QVERIFY(flattened.ok);
        const QImage white = decodeDataUrl(flattened.dataUrl);
        const QRgb corner = white.pixel(0, 0);
        QVERIFY2(qRed(white.pixel(8, 8)) >= 240 && qGreen(white.pixel(8, 8)) >= 240
                && qBlue(white.pixel(8, 8)) >= 240,
            QByteArray::number(qGray(corner)));
    }

    void prepareImageRejectsOverBudgetRasters()
    {
        // Reference cap: decoded width*height <= 24 000 000 (lib.ts:97).
        QImage oversized(4900, 4900, QImage::Format_ARGB32);
        oversized.fill(QColor(7, 84, 200));
        QByteArray bytes;
        {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            oversized.save(&buffer, "PNG");
        }
        const auto result = ChatBackground::prepareImage(bytes, QStringLiteral("image/png"));
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("size"));
    }

    void qmlFacingApplyResolveAndFilePipeline()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings ini(dir.filePath("backgrounds.ini"), QSettings::IniFormat);
        ini.setFallbacksEnabled(false);
        ChatBackground backgrounds(&ini);
        QVERIFY(backgrounds.backgroundUrl(QStringLiteral("stream:2f1a")).isEmpty()); // no owner
        backgrounds.setOwner(QStringLiteral("owner-a"));
        QVERIFY(backgrounds.applyPreset(QStringLiteral("stream:2f1a"),
            QStringLiteral("sea"), QStringLiteral("all")));
        QCOMPARE(backgrounds.selectedPreset(QStringLiteral("stream:2f1a")),
            QStringLiteral("sea"));
        QCOMPARE(backgrounds.backgroundUrl(QStringLiteral("stream:2f1a")),
            QStringLiteral("qrc:/workspace/chat-background/sea.svg"));
        QVERIFY(!backgrounds.applyPreset(QStringLiteral("stream:2f1a"),
            QStringLiteral("nope"), QStringLiteral("all")));
        // Scope "chat" needs a well-formed chatKey; "all" is always valid.
        QVERIFY(!backgrounds.applyPreset(QString{}, QStringLiteral("forest"),
            QStringLiteral("chat")));
        QVERIFY(backgrounds.applyPreset(QStringLiteral("topic:9c"),
            QStringLiteral("forest"), QStringLiteral("chat")));
        QCOMPARE(backgrounds.selectedPreset(QStringLiteral("topic:9c")),
            QStringLiteral("forest"));
        // Only a JPEG data URL inside the stored cap is accepted (lib.ts:24).
        QVERIFY(!backgrounds.applyImage(QStringLiteral("stream:2f1a"),
            QStringLiteral("data:image/png;base64,AUJj"), QStringLiteral("chat")));
        QVERIFY(!backgrounds.applyImage(QStringLiteral("stream:2f1a"),
            QStringLiteral("not a url"), QStringLiteral("all")));
        const auto prepared = ChatBackground::prepareImage(
            rasterFixture(8, 4, QColor(9, 9, 180)), QStringLiteral("image/png"));
        QVERIFY(prepared.ok);
        QVERIFY(backgrounds.applyImage(QStringLiteral("stream:2f1a"), prepared.dataUrl,
            QStringLiteral("chat")));
        QCOMPARE(backgrounds.customImage(QStringLiteral("stream:2f1a")), prepared.dataUrl);
        QCOMPARE(backgrounds.selectedPreset(QStringLiteral("stream:2f1a")), QString{});
        // "Default background": the reset applies per scope like every other value.
        QVERIFY(backgrounds.applyDefault(QStringLiteral("stream:2f1a"),
            QStringLiteral("chat")));
        QVERIFY(backgrounds.customImage(QStringLiteral("stream:2f1a")).isEmpty());
        QCOMPARE(backgrounds.selectedPreset(QStringLiteral("stream:2f1a")),
            QString{}); // the explicit chat default wins over the global preset
        // prepareImageFromFile: extension drives the declared MIME; anything
        // unreadable maps to the decode line (ui.tsx catch branch).
        const QString pngPath = dir.filePath("pic.png");
        QFile png(pngPath);
        QVERIFY(png.open(QIODevice::WriteOnly));
        png.write(rasterFixture(20, 10, QColor(30, 200, 30)));
        png.close();
        const auto fromFile = backgrounds.prepareImageFromFile(
            QStringLiteral("file://") + pngPath);
        QVERIFY(fromFile.value(QStringLiteral("ok")).toBool());
        QVERIFY(fromFile.value(QStringLiteral("dataUrl")).toString()
                    .startsWith(QStringLiteral("data:image/jpeg;base64,")));
        const QString txtPath = dir.filePath("note.txt");
        QVERIFY(QFile::copy(pngPath, txtPath));
        QCOMPARE(backgrounds.prepareImageFromFile(txtPath).value(QStringLiteral("error")),
            QStringLiteral("type"));
        QCOMPARE(backgrounds.prepareImageFromFile(dir.filePath("missing.png"))
                     .value(QStringLiteral("error")),
            QStringLiteral("decode"));
    }
};

QTEST_GUILESS_MAIN(NativeChatBackgroundTest)
#include "native-chat-background-test.moc"
