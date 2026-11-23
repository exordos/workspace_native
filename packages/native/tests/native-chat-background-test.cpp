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
    // Bug #678 fixture: stored raster 80x40 with a 20x10 red block at the
    // raw top-left and EXIF Orientation 6 (rotate 90 degrees clockwise for
    // display, so the intended view is 40x80 with the block at the TOP-RIGHT).
    // q95, 4:4:4, sha256 cac2a140dada9a578099c9e95d406f8fc4e451e2f2faeb5f8d771e548df1c893.
    static QByteArray exifOrient6Jpeg()
    {
        return QByteArray::fromBase64(
            "/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYAAAAAAAD/"
            "2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoK"
            "BggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoK"
            "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAoAFADAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEA"
            "AAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJx"
            "FDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNk"
            "ZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJ"
            "ytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQF"
            "BgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMz"
            "UvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3"
            "eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna"
            "4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD4vr+Uz/fwKAP1S/4Nmv8Amtn/AHLf/uUr"
            "9W8Mf+Yv/uH/AO3n8A/Tm/5p/wD7m/8A3WP1Sr9WP4BCgAoAKACgAoAKAPlb/hyd/wAEx/8Ao2f/"
            "AMvPWv8A5Mr5T/Ujhf8A6B//ACef/wAkfv8A/wATR+Ov/Q3/APLfC/8AykP+HJ3/AATH/wCjZ/8A"
            "y89a/wDkyj/Ujhf/AKB//J5//JB/xNH46/8AQ3/8t8L/APKT1X9mb9ib9mP9jv8Atv8A4Zx+Gf8A"
            "wjv/AAkX2b+2f+Jze3n2j7P5vk/8fU0mzb58v3cZ3c5wMerlmR5Xk3P9Tp8nPa+sne17bt93sfAc"
            "deKPHfiV9X/1jxft/Yc/s/3dKHLz8vP/AA4Qvfkj8V7W0td39Ur1j8/CgAoAKACgAoAKACgAoAKA"
            "CgAoAKACgAoAKACgAoAKACgAoAKACgAoAKACgAoAKACgAoAKACgAoAKACgAoAKAP/9k=");
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

    // Bug #677: preferences whose serialized form exceeds the 4,000,000-char
    // cap are a storage error that must keep the PREVIOUS configuration -
    // both the stored file and the live in-memory values. The oversized
    // payload here is assembled from individually valid images.
    void overCapApplyKeepsPreviousLiveAndStoredConfig()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings ini(dir.filePath("backgrounds.ini"), QSettings::IniFormat);
        ini.setFallbacksEnabled(false);
        ChatBackground backgrounds(&ini);
        backgrounds.setOwner(QStringLiteral("owner-cap"));
        const ChatBackground::Value mountains{
            ChatBackground::Kind::Preset, QStringLiteral("mountains")};
        const ChatBackground::Value sea{
            ChatBackground::Kind::Preset, QStringLiteral("sea")};
        QVERIFY(backgrounds.applyAll(mountains));
        // Exactly at the per-image stored cap: valid on its own.
        const QString bigUrl = QStringLiteral("data:image/jpeg;base64,")
            + QString(1'998'990 - 25, QLatin1Char('A'));
        const ChatBackground::Value big{ChatBackground::Kind::Image, bigUrl};
        QVERIFY(backgrounds.applyChat(QStringLiteral("stream:aaaa"), big));
        QVERIFY(backgrounds.applyChat(QStringLiteral("stream:bbbb"), big));
        // The third image pushes the combined payload over the cap: rejected.
        QVERIFY(!backgrounds.applyChat(QStringLiteral("stream:cccc"), big));
        QCOMPARE(backgrounds.error(), QStringLiteral("storage"));
        // The rejected photo must not go live in the chat it was aimed at...
        QVERIFY(backgrounds.resolve(QStringLiteral("stream:cccc")).kind
            == ChatBackground::Kind::Preset);
        QCOMPARE(backgrounds.resolve(QStringLiteral("stream:cccc")).text,
            QStringLiteral("mountains"));
        // A subsequent valid edit still fits alongside the stored configuration.
        QVERIFY(backgrounds.applyChat(QStringLiteral("stream:dddd"), sea));
        // A fresh instance restores exactly the accepted writes.
        ChatBackground reopened(&ini);
        reopened.setOwner(QStringLiteral("owner-cap"));
        QVERIFY(reopened.resolve(QStringLiteral("stream:aaaa")).kind
            == ChatBackground::Kind::Image);
        QVERIFY(reopened.resolve(QStringLiteral("stream:bbbb")).kind
            == ChatBackground::Kind::Image);
        QCOMPARE(reopened.resolve(QStringLiteral("stream:cccc")).text,
            QStringLiteral("mountains"));
        QCOMPARE(reopened.resolve(QStringLiteral("stream:dddd")).text,
            QStringLiteral("sea"));
        // Scope "all" with an in-cap value is a legal commit that clears
        // every override (reference apply scope semantics) - live and stored.
        QVERIFY(backgrounds.applyAll(mountains));
        QCOMPARE(backgrounds.resolve(QStringLiteral("stream:aaaa")).text,
            QStringLiteral("mountains"));
        ChatBackground relaunched(&ini);
        relaunched.setOwner(QStringLiteral("owner-cap"));
        QCOMPARE(relaunched.resolve(QStringLiteral("stream:dddd")).text,
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

    void prepareImageHonorsExifOrientation()
    {
        // Bug #678: an orientation-aware reader returns 40x80 for this JPEG
        // (80x40 raster + Orientation 6), while the pre-fix pipeline returned
        // a JPEG with the unrotated 80x40 raster. The corrected pixels must
        // be BAKED into the saved output, so the plain (non-transforming)
        // decodeDataUrl below already shows the intended display orientation.
        const QByteArray fixture = exifOrient6Jpeg();
        const auto result = ChatBackground::prepareImage(fixture, QStringLiteral("image/jpeg"));
        QVERIFY2(result.ok, qPrintable(result.error));
        const QImage out = decodeDataUrl(result.dataUrl);
        QCOMPARE(out.width(), 40);
        QCOMPARE(out.height(), 80);
        const auto isRed = [](QRgb p) {
            return qRed(p) > 150 && qGreen(p) < 100 && qBlue(p) < 100;
        };
        // Rotating the raster 90 degrees clockwise moves the raw top-left
        // block (x in [0,20), y in [0,10)) to x' in [30,40), y' in [0,20).
        QVERIFY2(isRed(out.pixel(35, 10)), QByteArray::number(out.pixel(35, 10)));
        QVERIFY2(!isRed(out.pixel(2, 2)), QByteArray::number(out.pixel(2, 2)));
        QVERIFY2(!isRed(out.pixel(5, 75)), QByteArray::number(out.pixel(5, 75)));
        QVERIFY2(!isRed(out.pixel(37, 70)), QByteArray::number(out.pixel(37, 70)));
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
