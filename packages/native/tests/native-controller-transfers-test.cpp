#include "clipboard-snapshot-budget.h"
#include "native-markdown.h"
#include "support/native-test-harness.h"

#include <QDateTime>
#include <QThread>

class DownloadDesktopProbe final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
public:
    QList<QUrl> opened;
    QStringList revealed;
public slots:
    void open(const QUrl &url) { opened.append(url); }
    void ShowItems(const QStringList &uris, const QString &) { revealed = uris; }
};

class ControllerTransfersTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void mislabeledImageNeverUploadsOrChangesTheDraft()
    {
        Fixture fixture;
        FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        QFile input(directory.filePath("wrong.png"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("not a PNG image"); input.close();
        int uploads = 0;
        fixture.files = [&](const Request &) { ++uploads; return Response{QJsonObject{}, 503}; };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Keep this draft");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(uploads, 0);
        QVERIFY(fixture.controller.error().contains("image file type does not match"));
        QCOMPARE(fixture.controller.draft(), QString("Keep this draft"));
        QVERIFY(!fixture.controller.canSend());
        const auto rows = fixture.controller.uploadEntries(); QCOMPARE(rows.size(), 1);
        QVERIFY(!rows.first().toMap().value("retryable").toBool());
        fixture.controller.removeUpload(rows.first().toMap().value("id").toString());
        QVERIFY(fixture.controller.canSend());
    }

    void uploadNoticeDoesNotHideDownloadProgressOrSavedDestination()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write(bytes); input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201, 0, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.uploadEntries().first().toMap().value("status").toString(), QString("ready"));
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download"))
                return Response{QJsonObject{}}; // Actual body is the two bytes "{}".
            return Response{QJsonObject{{"uuid", returnedId}, {"name", "report.txt"}, {"content_type", "text/plain"},
                {"size_bytes", 2}, {"hash", QString::fromLatin1(QCryptographicHash::hash("{}", QCryptographicHash::Sha256).toHex())}}};
        };
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("saved.bin")));
        QVERIFY(fixture.controller.notice().contains("Downloading"));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY2(fixture.controller.notice().contains("saved.bin"), qPrintable(fixture.controller.notice()));
        QFile output(directory.filePath("saved.bin")); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), "{}");
    }

    void synchronousUploadFailuresDrainWithoutRecursionOrStaleGates()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Still sendable");
        QVariantList selection;
        for (int i = 0; i < 2048; ++i) selection.append(QUrl::fromLocalFile("/nonexistent/cassi-native-attachment"));
        const auto before = fixture.server.requests.size();
        fixture.controller.attachFiles(selection);
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY(!fixture.controller.canSend());
        QCOMPARE(fixture.controller.uploadEntries().size(), 2048);
        QCOMPARE(fixture.controller.draft(), "Still sendable");
        QCOMPARE(fixture.server.requests.size(), before);
    }

    void attachmentBatchQueuesAllFilesWithAtMostFiveActiveRequests()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("Queued attachment\n");
        QTemporaryDir directory;
        QVariantList selection;
        for (int i = 0; i < 11; ++i) {
            const auto folder = directory.filePath(QString::number(i));
            QVERIFY(QDir().mkpath(folder));
            QFile input(folder + "/report.txt");
            QVERIFY(input.open(QIODevice::WriteOnly));
            QCOMPARE(input.write(bytes), bytes.size());
            selection.append(QUrl::fromLocalFile(input.fileName()));
        }
        int active = 0, maximumActive = 0, started = 0;
        fixture.files = [&](const Request &) {
            maximumActive = std::max(maximumActive, ++active);
            ++started;
            QTimer::singleShot(120, &fixture.server, [&] { --active; });
            return Response{QJsonObject{{"uuid", historyMessage(started).value("uuid")}, {"stream_uuid", streamId},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201, 120, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Message");
        fixture.controller.attachFiles(selection);
        QVERIFY(!fixture.controller.canSend());
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(started, 11);
        QCOMPARE(maximumActive, 5);
        QCOMPARE(fixture.controller.draft(), QString("Message"));
        QCOMPARE(fixture.controller.uploadEntries().size(), 11);
        for (const auto &row : fixture.controller.uploadEntries()) QCOMPARE(row.toMap().value("status").toString(), QString("ready"));
        QVERIFY(fixture.controller.canSend());
    }

    void destroyingFileServiceClearsQueuedAndActiveControllerState()
    {
        Fixture fixture;
        auto files = std::make_unique<FileClient>(&fixture.api);
        fixture.controller.useFiles(files.get());
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("attachment\n"); input.close();
        int started = 0;
        fixture.files = [&](const Request &) { ++started; return Response{QJsonObject{}, 500, 300, {}}; };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Original draft");
        QVariantList selection;
        for (int i = 0; i < 9; ++i) selection.append(QUrl::fromLocalFile(input.fileName()));
        fixture.controller.attachFiles(selection);
        QTRY_VERIFY(started >= 5);
        files.reset();
        QVERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.canSend());
        QVERIFY(!fixture.controller.canAttach());
        QCOMPARE(fixture.controller.draft(), "Original draft");
        const auto completed = started;
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(started, completed);
        QVERIFY(!fixture.controller.notice().contains("Uploading"));
    }

    void replacingFileServiceCancelsOnlyControllerOperations()
    {
        Fixture fixture;
        FileClient oldService(&fixture.api), newService(&fixture.api);
        fixture.controller.useFiles(&oldService);
        const QByteArray bytes("attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write(bytes); input.close();
        int started = 0;
        fixture.files = [&](const Request &) {
            ++started;
            return Response{QJsonObject{{"uuid", historyMessage(started).value("uuid")}, {"stream_uuid", streamId},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201, 150, {}};
        };
        QVERIFY(fixture.start());
        int foreignCompletions = 0;
        bool foreignOk = false;
        oldService.upload(input.fileName(), streamId, {}, [&](const FileResult &result) { ++foreignCompletions; foreignOk = result.ok(); });
        fixture.controller.setDraft("Original draft");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_COMPARE(started, 2);
        fixture.controller.useFiles(&newService);
        QVERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.canSend());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QTRY_COMPARE(foreignCompletions, 1);
        QVERIFY(foreignOk);
        QCOMPARE(fixture.controller.draft(), QString("Original draft"));
        QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        QCOMPARE(fixture.controller.uploadEntries().first().toMap().value("status").toString(), QString("ready"));
        QCOMPARE(started, 3);
    }

    void uploadedImageParagraphBoundariesAndLegacyFileVideo_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("initialDraft");
        QTest::addColumn<QString>("typedAfterFirst");
        QTest::addColumn<bool>("secondImage");
        QTest::addColumn<QString>("expectedPattern");
        QTest::newRow("image-empty") << QString("image") << QString() << QString() << false << QString("@FIRST@\n\n");
        QTest::newRow("image-caption") << QString("image") << QString("Caption") << QString() << false << QString("Caption\n\n@FIRST@\n\n");
        QTest::newRow("image-trailing-one") << QString("image") << QString("Caption\n") << QString() << false << QString("Caption\n\n@FIRST@\n\n");
        QTest::newRow("image-trailing-two") << QString("image") << QString("Caption\n\n") << QString() << false << QString("Caption\n\n@FIRST@\n\n");
        QTest::newRow("image-subsequent-caption") << QString("image") << QString() << QString("Later caption") << false << QString("@FIRST@\n\nLater caption");
        QTest::newRow("image-subsequent-image") << QString("image") << QString() << QString() << true << QString("@FIRST@\n\n@SECOND@\n\n");
        QTest::newRow("image-subsequent-caption-and-image") << QString("image") << QString() << QString("Later caption") << true << QString("@FIRST@\n\nLater caption\n\n@SECOND@\n\n");
        QTest::newRow("file-legacy-two-newlines") << QString("file") << QString("Caption\n\n") << QString() << false << QString("Caption\n\n@FIRST@\n");
        QTest::newRow("video-legacy-one-newline") << QString("video") << QString("Caption") << QString() << false << QString("Caption\n@FIRST@\n");
    }

    void uploadedImageParagraphBoundariesAndLegacyFileVideo()
    {
        QFETCH(QString, kind); QFETCH(QString, initialDraft); QFETCH(QString, typedAfterFirst);
        QFETCH(bool, secondImage); QFETCH(QString, expectedPattern);
        // Same public PNG as the actual Main received/upload test. The MP4
        // sample is a synthetic MIME signature only, not playback acceptance.
        const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a0000000d4948445200000060000000400806000000e534720e000000ad4944415478daedd1310d80301440c19a68d28d818da4766a017d4c18a8061430b023004474f81d6e7806dea5be5ddf48f75243cb470b6df45f02000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000260058f727b4f72ca1010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001300fc64110b8532ff49270000000049454e44ae426082");
        const QByteArray bytes = kind == "image" ? png : kind == "video"
            ? QByteArray::fromHex("000000186674797069736f6d0000000069736f6d6d703432") : QByteArray("plain fixture\n");
        const QString name = kind == "image" ? "image.png" : kind == "video" ? "video.mp4" : "report.txt";
        const QString mime = kind == "image" ? "image/png" : kind == "video" ? "video/mp4" : "text/plain";
        const QString firstId = returnedId, secondId = "99999999-9999-4999-8999-999999999994";
        const auto linkFor = [&](const QString &id) {
            const auto prefix = kind == "image" ? QString("!") : QString{};
            return prefix + '[' + name + "](<urn:" + kind + ':' + id + "?name=" + name
                + "&content_type=" + QString::fromLatin1(QUrl::toPercentEncoding(mime))
                + (kind == "image" ? QStringLiteral("&w=96&h=64") : QString{})
                + "&size=" + QString::number(bytes.size()) + ">)";
        };
        Q_UNUSED(expectedPattern);
        QString wire; int sends = 0;
        int uploads = 0, unexpected = 0;
        bool headersOkay = true;
        Fixture fixture;
        FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++sends; wire = request.body.value("payload").toObject().value("content").toString(); return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{}};
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile input(directory.filePath(name)); QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(bytes), qint64(bytes.size())); input.close();
        if (kind == "image") {
            QCOMPARE(bytes.size(), qsizetype(230));
            QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                QByteArray("9fb39a2b2737f88bdf0b383c4fcea4b0f3636e584b280f10afd18444739ed1f9"));
        }
        fixture.files = [&](const Request &request) -> Response {
            if (request.method != "POST" || request.target.path() != "/api/workspace/v1/messenger/files/") {
                ++unexpected; return Response{QJsonObject{}, 405};
            }
            ++uploads;
            headersOkay = headersOkay
                && request.headers.value("authorization") == QByteArray("Bearer synthetic-project-token")
                && request.headers.value("cookie").isEmpty()
                && request.headers.value("content-type").startsWith("multipart/form-data;")
                && request.headers.value("content-length").toLongLong() > bytes.size();
            return Response{QJsonObject{{"uuid", uploads == 1 ? firstId : secondId}, {"stream_uuid", streamId},
                {"name", name}, {"content_type", mime}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.selectedStream(), streamId); QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(fixture.controller.canAttach());
        fixture.controller.setDraft(initialDraft);
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy()); QVERIFY(!fixture.controller.canSend());
        QTRY_COMPARE(uploads, 1); QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY2(fixture.controller.error().isEmpty(), qPrintable(fixture.controller.error()));
        QVERIFY(headersOkay); QCOMPARE(unexpected, 0);
        QCOMPARE(fixture.controller.draft(), initialDraft);
        QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        // Public draft input, after a real first upload. No completion signal,
        // private state or fabricated upload result is injected.
        if (!typedAfterFirst.isEmpty()) fixture.controller.setDraft(fixture.controller.draft() + typedAfterFirst);
        if (secondImage) {
            fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
            QVERIFY(fixture.controller.fileBusy()); QVERIFY(!fixture.controller.canSend());
            QTRY_COMPARE(uploads, 2); QTRY_VERIFY(!fixture.controller.fileBusy());
            QVERIFY2(fixture.controller.error().isEmpty(), qPrintable(fixture.controller.error()));
            QCOMPARE(fixture.controller.uploadEntries().size(), 2);
        }
        QCOMPARE(uploads, secondImage ? 2 : 1); QVERIFY(headersOkay); QCOMPARE(unexpected, 0);
        QCOMPARE(fixture.controller.selectedStream(), streamId); QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(fixture.controller.canSend());
        const auto visible = initialDraft + typedAfterFirst;
        QCOMPARE(fixture.controller.draft(), visible);
        const auto links = linkFor(firstId) + (secondImage ? "\n" + linkFor(secondId) : QString{});
        const auto expected = links + (visible.trimmed().isEmpty() ? QString{} : "\n" + visible.trimmed());
        fixture.controller.send(); QTRY_COMPARE(sends, 1); QCOMPARE(wire, expected);
        NativeMarkdown markdown;
        const auto parts = markdown.render(wire).value("parts").toList();
        QStringList imageIds;
        for (const auto &part : parts)
            if (part.toMap().value("kind") == "image") imageIds.append(part.toMap().value("id").toString());
        QCOMPARE(imageIds, kind == "image" ? (secondImage ? QStringList{firstId, secondId} : QStringList{firstId}) : QStringList{});
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
    }

    void attachmentCardsExposeIndependentPendingFiles()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        QFile first(directory.filePath("first.txt"));
        QFile second(directory.filePath("second.txt"));
        QVERIFY(first.open(QIODevice::WriteOnly)); first.write("first"); first.close();
        QVERIFY(second.open(QIODevice::WriteOnly)); second.write("second"); second.close();
        QList<std::function<void()>> completions;
        fixture.files = [&](const Request &) {
            Response result{QJsonObject{}, 503};
            result.defer = [&](auto send) { completions.append(std::move(send)); };
            return result;
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Keep editing");
        fixture.controller.attachFiles({QUrl::fromLocalFile(first.fileName()), QUrl::fromLocalFile(second.fileName())});
        QTRY_COMPARE(completions.size(), 2);
        const auto cards = fixture.controller.property("uploadEntries").toList();
        QCOMPARE(cards.size(), 2);
        QCOMPARE(cards.at(0).toMap().value("name").toString(), QString("first.txt"));
        QCOMPARE(cards.at(1).toMap().value("name").toString(), QString("second.txt"));
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.setDraft("Still editing");
        QCOMPARE(fixture.controller.draft(), QString("Still editing"));
        for (auto &complete : completions) complete();
    }

    void attachmentCardsCancelRetryAndRemoveIndividually()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        QFile first(directory.filePath("one.txt")); QFile second(directory.filePath("two.txt"));
        QVERIFY(first.open(QIODevice::WriteOnly)); first.write("one"); first.close();
        QVERIFY(second.open(QIODevice::WriteOnly)); second.write("two"); second.close();
        std::function<void()> held; int requests = 0;
        fixture.files = [&](const Request &) {
            ++requests;
            if (requests == 1) {
                Response response{QJsonObject{}, 503}; response.defer = [&](auto send) { held = std::move(send); };
                return response;
            }
            if (requests == 2) return Response{QJsonObject{}, 503};
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", "two.txt"}, {"content_type", "text/plain"}, {"size_bytes", 3},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("two", QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Keep this text");
        fixture.controller.attachFiles({QUrl::fromLocalFile(first.fileName()), QUrl::fromLocalFile(second.fileName())});
        QTRY_COMPARE(requests, 2); QTRY_VERIFY(bool(held));
        const auto entries = [&] { return fixture.controller.property("uploadEntries").toList(); };
        QTRY_COMPARE(entries().size(), 2);
        QTRY_COMPARE(entries().at(1).toMap().value("status").toString(), QString("error"));
        const auto firstId = entries().at(0).toMap().value("id").toString();
        const auto secondId = entries().at(1).toMap().value("id").toString();
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.removeUpload(firstId);
        QCOMPARE(entries().size(), 1); QCOMPARE(entries().first().toMap().value("id").toString(), secondId);
        QVERIFY(!fixture.controller.canSend()); // An error must be retried or removed before sending.
        held(); held = {};
        fixture.controller.retryUpload(secondId);
        QTRY_COMPARE(requests, 3);
        QTRY_COMPARE(entries().first().toMap().value("status").toString(), QString("ready"));
        QVERIFY2(fixture.controller.error().isEmpty(), qPrintable(fixture.controller.error()));
        QCOMPARE(fixture.controller.draft(), QString("Keep this text"));
        fixture.controller.removeUpload(secondId);
        QVERIFY(entries().isEmpty());
        QVERIFY(!fixture.controller.draft().contains("urn:file:" + returnedId));
        QVERIFY(fixture.controller.draft().contains("Keep this text"));
    }

    void referenceReadyAttachmentDoesNotChangeVisibleDraft()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("report"); input.close();
        fixture.files = [&](const Request &) { return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
            {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 6},
            {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201}; };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Caption");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.draft(), QString("Caption"));
        QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        QCOMPARE(fixture.controller.uploadEntries().first().toMap().value("status").toString(), QString("ready"));
    }

    void removingAnUnsentReadyUploadDeletesOnlyThatFile()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("report"); input.close();
        int deletes = 0; QString deletedPath;
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") { deletedPath = request.target.path(); ++deletes; return Response{QJsonObject{}, 204}; }
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 6},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Caption");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy()); QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        fixture.controller.removeUpload(fixture.controller.uploadEntries().first().toMap().value("id").toString());
        QTRY_COMPARE(deletes, 1); QCOMPARE(deletedPath, QString("/api/workspace/v1/messenger/files/") + returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Caption"));
    }

    void changingConversationDiscardsOnlyUnsentUpload()
    {
        Fixture fixture; fixture.extraTopic = true; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("report"); input.close();
        int deletes = 0;
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") { ++deletes; return Response{QJsonObject{}, 204}; }
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 6},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Original text");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy()); QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        QTRY_COMPARE(deletes, 1);
        QVERIFY(fixture.controller.uploadEntries().isEmpty());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.draft(), QString("Original text")); QVERIFY(fixture.controller.uploadEntries().isEmpty());
    }

    void uploadCleanupRejectsChangedGeneration()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("report"); input.close();
        int deletes = 0;
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") { ++deletes; return Response{QJsonObject{}, 204}; }
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 6},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QVERIFY(fixture.start()); fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy()); QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        const auto id = fixture.controller.uploadEntries().first().toMap().value("id").toString();
        fixture.api.setAccessToken("replacement-token");
        fixture.controller.removeUpload(id);
        QTest::qWait(100); QCOMPARE(deletes, 0);
    }

    void inlineImageAliasStaysOutOfPersistentDraftAndSurvivesUncertainSend()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        const auto path = directory.filePath("diagram.png"); QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::red); QVERIFY(image.save(path)); QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly));
        const auto bytes = input.readAll(); input.close();
        int deletes = 0; QString wire;
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") { ++deletes; return Response{QJsonObject{}, 204}; }
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "diagram.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
        };
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { wire = request.body.value("payload").toObject().value("content").toString(); return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId)); fixture.controller.setDraft("Before after");
        fixture.controller.attachFiles({QUrl::fromLocalFile(path)}); QTRY_VERIFY(!fixture.controller.fileBusy());
        const auto row = fixture.controller.uploadEntries().first().toMap();
        QCOMPARE(row.value("alias").toString(), QString("![diagram.png]"));
        fixture.controller.setDraft("Before ![diagram.png] after");
        QCOMPARE(fixture.controller.draft(), QString("Before ![diagram.png] after"));
        QCOMPARE(drafts.text(streamId, topicId), QString("Before  after"));
        QVERIFY(fixture.controller.uploadEntries().first().toMap().value("inText").toBool());
        fixture.controller.removeUploadFromText(row.value("id").toString());
        QCOMPARE(fixture.controller.draft(), QString("Before  after")); QCOMPARE(deletes, 0);
        fixture.controller.setDraft("Before ![diagram.png] after");
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send(); QTRY_VERIFY(!wire.isEmpty());
        QCOMPARE(wire.count("urn:image:" + returnedId), 1); QVERIFY(wire.startsWith("Before ![diagram.png]("));
        QVERIFY(wire.contains("&w=8&h=8&size="));
        QVERIFY(wire.endsWith(" after")); QCOMPARE(deletes, 0);
        const auto owner = drafts.owner(); bool flushed = false; drafts.flush(this, [&](bool ok) { flushed = ok; }); QTRY_VERIFY(flushed);
        storage.shutdown(); QTRY_VERIFY(storage.stopped()); NativeDraftStore store(directory.filePath("drafts"));
        QCOMPARE(store.open(owner), NativeDraftService::Failure::None);
        QCOMPARE(store.outgoing().rows.first().payload.value("content").toString(), wire);
        QVERIFY(!store.latest(streamId, topicId).record->text.contains("![diagram.png]"));
    }

    void removingFirstOfTwoSameNamedImagesKeepsSecondInline()
    {
        QTemporaryDir directory;
        const auto writeImage = [&](const QString &subdir, const QColor &color) {
            const QString path = directory.filePath(subdir + "/diagram.png");
            if (!QDir().mkpath(QFileInfo(path).absolutePath())) return QString{};
            QImage image(8, 8, QImage::Format_ARGB32);
            image.fill(color);
            return image.save(path) ? path : QString{};
        };
        const QString firstPath = writeImage("first", Qt::red);
        const QString secondPath = writeImage("second", Qt::blue);
        QFile firstInput(firstPath), secondInput(secondPath);
        QVERIFY(firstInput.open(QIODevice::ReadOnly));
        QVERIFY(secondInput.open(QIODevice::ReadOnly));
        const auto firstBytes = firstInput.readAll(), secondBytes = secondInput.readAll();
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        const QString secondId = "99999999-9999-4999-8999-999999999994";
        std::function<void()> completeFirst;
        QString wire;
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") return Response{QJsonObject{}, 204};
            const bool first = request.rawBody.contains(firstBytes);
            const auto &bytes = first ? firstBytes : secondBytes;
            if (!request.rawBody.contains(bytes)) return Response{QJsonObject{}, 400};
            Response response{QJsonObject{{"uuid", first ? returnedId : secondId}, {"stream_uuid", streamId}, {"name", "diagram.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
            if (first) response.defer = [&](std::function<void()> reply) { completeFirst = std::move(reply); };
            return response;
        };
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { wire = request.body.value("payload").toObject().value("content").toString(); return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.attachFiles({QUrl::fromLocalFile(firstPath), QUrl::fromLocalFile(secondPath)});
        QTRY_VERIFY(completeFirst && fixture.controller.uploadEntries().last().toMap().value("status") == "ready");
        const auto aliasWhileFirstIsPending = fixture.controller.uploadEntries().last().toMap().value("alias").toString();
        completeFirst();
        QCOMPARE(aliasWhileFirstIsPending, QString("![diagram.png (2)]"));
        QTRY_VERIFY(!fixture.controller.fileBusy()); QCOMPARE(fixture.controller.uploadEntries().size(), 2);
        const auto first = fixture.controller.uploadEntries().first().toMap().value("id").toString();
        QCOMPARE(fixture.controller.uploadEntries().last().toMap().value("alias").toString(), QString("![diagram.png (2)]"));
        fixture.controller.setDraft("Before ![diagram.png (2)] after"); fixture.controller.removeUpload(first);
        QVERIFY(fixture.controller.uploadEntries().first().toMap().value("inText").toBool());
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send(); QTRY_VERIFY(!wire.isEmpty());
        QVERIFY(wire.startsWith("Before ![diagram.png](")); QVERIFY(wire.contains("urn:image:" + secondId));
        QVERIFY(!wire.contains("![diagram.png (2)]"));
    }

    void inlineImageInEditedMessageIsSerializedOnce()
    {
        QTemporaryDir directory; const auto path = directory.filePath("diagram.png");
        QImage image(8, 8, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(path));
        QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly)); const auto bytes = input.readAll();
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "diagram.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
        };
        auto note = message("Original message"); QString wire;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") {
                wire = request.body.value("payload").toObject().value("content").toString();
                note["payload"] = request.body.value("payload"); return Response{note};
            }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{note}} : Response{note};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Unrelated draft");
        QVERIFY(fixture.controller.beginEditMessage(messageId)); fixture.controller.attachFiles({QUrl::fromLocalFile(path)});
        QTRY_VERIFY(!fixture.controller.fileBusy()); fixture.controller.setDraft("Before ![diagram.png] after");
        QVERIFY(fixture.controller.canSaveEdit()); fixture.controller.submitEditMessage(); QTRY_VERIFY(!wire.isEmpty());
        QCOMPARE(wire.count("urn:image:" + returnedId), 1); QVERIFY(wire.startsWith("Before ![diagram.png]("));
        QVERIFY(wire.endsWith(" after")); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("Unrelated draft"));
    }

    void retryInlineImageKeepsClientIdentityWithoutDurableStore()
    {
        QTemporaryDir directory; const auto path = directory.filePath("diagram.png");
        QImage image(8, 8, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(path));
        QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly)); const auto bytes = input.readAll();
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "diagram.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QStringList ids;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { ids.append(request.body.value("uuid").toString()); return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.attachFiles({QUrl::fromLocalFile(path)});
        QTRY_VERIFY(!fixture.controller.fileBusy()); fixture.controller.setDraft("Caption ![diagram.png]");
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send(); QTRY_COMPARE(ids.size(), 1);
        QTRY_VERIFY(!fixture.controller.busy()); QTRY_VERIFY(fixture.controller.canSend());
        fixture.controller.send(); QTRY_COMPARE(ids.size(), 2); QCOMPARE(ids[0], ids[1]);
    }

    void confirmedInlineImageDoesNotLeaveAStaleMemoryDraft()
    {
        QTemporaryDir directory; const auto path = directory.filePath("diagram.png");
        QImage image(8, 8, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(path));
        QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly)); const auto bytes = input.readAll();
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "diagram.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QJsonObject saved;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                const auto id = request.body.value("uuid").toString();
                const auto placement = QUuid::createUuidV5(QUuid(topicId), id.toUtf8()).toString(QUuid::WithoutBraces);
                saved = message(request.body.value("payload").toObject().value("content").toString(), placement);
                return Response{saved};
            }
            if (!saved.isEmpty() && request.target.path().endsWith(saved.value("uuid").toString())) return Response{saved};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.attachFiles({QUrl::fromLocalFile(path)});
        QTRY_VERIFY(!fixture.controller.fileBusy()); fixture.controller.setDraft("Caption ![diagram.png]");
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_VERIFY(!saved.isEmpty()); QTRY_VERIFY(!fixture.controller.busy());
        QTRY_VERIFY(fixture.controller.uploadEntries().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString{});
    }

    void removingUploadClearsItsAliasFromInactiveReply()
    {
        const QString first = "11111111-1111-4111-8111-111111111111", second = "22222222-2222-4222-8222-222222222222";
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        const auto one = message("First", first), two = message("Second", second);
        fixture.messages = [&](const Request &) { return Response{QJsonArray{one, two}}; };
        QTemporaryDir directory; const auto path = directory.filePath("diagram.png");
        QImage image(8, 8, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(path));
        QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly)); const auto bytes = input.readAll();
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") return Response{QJsonObject{}, 204};
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "diagram.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginReply(first));
        fixture.controller.attachFiles({QUrl::fromLocalFile(path)}); QTRY_VERIFY(!fixture.controller.fileBusy());
        const auto card = fixture.controller.uploadEntries().first().toMap().value("id").toString();
        fixture.controller.setDraft("first ![diagram.png] answer");
        const auto firstTab = fixture.controller.replyTabs().first().toMap().value("id").toString();
        QVERIFY(fixture.controller.addReplyTab(second)); fixture.controller.setDraft("second answer");
        fixture.controller.removeUpload(card);
        QVERIFY(fixture.controller.selectReplyTab(firstTab));
        QCOMPARE(fixture.controller.draft(), QString("first  answer"));
    }

    void oversizedWireWithAttachmentDoesNotSend()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("report"); input.close(); int posts = 0;
        fixture.files = [&](const Request &) { return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
            {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 6},
            {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201}; };
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") ++posts; return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft(QString(40000, QLatin1Char('a')));
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())}); QTRY_VERIFY(!fixture.controller.fileBusy());
        fixture.controller.send(); QTest::qWait(400); QCOMPARE(posts, 0);
        QCOMPARE(fixture.controller.draft().size(), 40000); QCOMPARE(fixture.controller.uploadEntries().size(), 1);
    }

    void clipboardFileUrlCannotUploadWithoutConfirmation()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("synthetic-private.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("synthetic"); input.close(); int posts = 0;
        fixture.files = [&](const Request &) { ++posts; return Response{QJsonObject{}, 503}; };
        auto *clipboard = QGuiApplication::clipboard(); auto *old = new QMimeData;
        if (const auto *previous = clipboard->mimeData()) for (const auto &format : previous->formats()) old->setData(format, previous->data(format));
        const auto restore = qScopeGuard([&] { clipboard->setMimeData(old); });
        QVERIFY(fixture.start()); auto *data = new QMimeData;
        data->setUrls({QUrl::fromLocalFile(input.fileName())}); clipboard->setMimeData(data);
        const bool accepted = fixture.controller.pasteAttachments(); QTest::qWait(400);
        QCOMPARE(posts, 0); QVERIFY(!accepted);
    }

    void destroyingControllerCancelsClipboardSnapshotPreparation_data()
    {
        QTest::addColumn<QString>("transition");
        QTest::newRow("destroy") << QString("destroy");
        QTest::newRow("logout") << QString("logout");
        QTest::newRow("cancel") << QString("cancel");
        QTest::newRow("edit") << QString("edit");
        QTest::newRow("edit-switch") << QString("edit-switch");
    }

    void destroyingControllerCancelsClipboardSnapshotPreparation()
    {
        if (!qEnvironmentVariableIsSet("CASSI_SLOW_CLIPBOARD_TEST")) QSKIP("Run by native-clipboard-lifecycle with controlled slow input");
        QFETCH(QString, transition);
        QTemporaryDir directory; QFile input(directory.filePath("cassi-slow-synthetic.bin"));
        QVERIFY(input.open(QIODevice::WriteOnly)); QVERIFY(input.resize(1024 * 1024)); input.close();
        auto *clipboard = QGuiApplication::clipboard(); auto *old = new QMimeData;
        if (const auto *previous = clipboard->mimeData()) for (const auto &format : previous->formats()) old->setData(format, previous->data(format));
        const auto restore = qScopeGuard([&] { clipboard->setMimeData(old); });
        auto fixture = std::make_unique<Fixture>(); auto files = std::make_unique<FileClient>(&fixture->api);
        fixture->controller.useFiles(files.get());
        if (transition == "edit" || transition == "edit-switch") fixture->messages = [](const Request &) -> Response { return {QJsonArray{message()}}; };
        QVERIFY(fixture->start());
        if (transition == "edit") QVERIFY(fixture->controller.beginEditMessage(messageId));
        QList<QUrl> urls; for (int i = 0; i < 10; ++i) urls.append(QUrl::fromLocalFile(input.fileName()));
        auto *data = new QMimeData; data->setUrls(urls); clipboard->setMimeData(data);
        QVERIFY(fixture->controller.prepareClipboardFiles());
        QTRY_VERIFY(!fixture->controller.clipboardFilesPreparing());
        QVERIFY(!fixture->controller.clipboardFilesPreview().isEmpty()); QTest::qWait(50);
        const QDir tasks("/proc/self/task");
        const auto ids = [&] { const auto list = tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot); return QSet<QString>(list.begin(), list.end()); };
        const auto before = ids();
        fixture->controller.setDraft("Draft while copying");
        fixture->controller.attachClipboardFiles();
        if (transition == "edit") QVERIFY(!fixture->controller.canSaveEdit());
        else QVERIFY(!fixture->controller.canSend());
        QSet<QString> started;
        QTRY_VERIFY(!(started = ids() - before).isEmpty());
        if (transition == "destroy") { fixture.reset(); files.reset(); }
        else if (transition == "logout") fixture->controller.logout();
        else if (transition == "edit-switch") QVERIFY(fixture->controller.beginEditMessage(messageId));
        else fixture->controller.cancelClipboardFiles();
        QTRY_VERIFY_WITH_TIMEOUT((ids() & started).isEmpty(), 500);
        if (fixture) {
            QVERIFY(!fixture->controller.clipboardFilesPreparing());
            QVERIFY(fixture->controller.clipboardFilesPreview().isEmpty());
        }
    }

    void clipboardDoesNotReadBeforeConfirmation()
    {
        if (!qEnvironmentVariableIsSet("CASSI_PRECONFIRM_TEST")) QSKIP("Run by native-clipboard-consent with synthetic input");
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; const auto path = directory.filePath("cassi-preconfirm-synthetic.bin"), marker = directory.filePath("cassi-preconfirm-read.marker");
        QFile input(path); QVERIFY(input.open(QIODevice::WriteOnly)); input.write("synthetic"); input.close();
        auto *clipboard = QGuiApplication::clipboard(); auto *old = new QMimeData;
        if (const auto *previous = clipboard->mimeData()) for (const auto &format : previous->formats()) old->setData(format, previous->data(format));
        const auto restore = qScopeGuard([&] { clipboard->setMimeData(old); });
        QVERIFY(fixture.start()); auto *data = new QMimeData; data->setUrls({QUrl::fromLocalFile(path)}); clipboard->setMimeData(data);
        QVERIFY(fixture.controller.prepareClipboardFiles()); QTRY_VERIFY(!fixture.controller.clipboardFilesPreparing());
        QVERIFY(!fixture.controller.clipboardFilesPreview().isEmpty());
        QVERIFY2(!QFileInfo::exists(marker), "The file content was read before the user confirmed its path");
        fixture.controller.cancelClipboardFiles();
    }

    void clipboardPathResolutionRaceCannotUploadAnotherFile()
    {
        if (!qEnvironmentVariableIsSet("CASSI_CLIPBOARD_PATH_RACE")) QSKIP("Run by native-clipboard-path-race with synthetic path replacement");
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; const auto selected = directory.filePath("cassi-race-selected.bin"), secret = directory.filePath("cassi-race-secret.bin");
        QFile input(selected), privateInput(secret);
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("benign-synthetic"); input.close();
        QVERIFY(privateInput.open(QIODevice::WriteOnly)); privateInput.write("private-synthetic"); privateInput.close();
        int posts = 0; QByteArray uploaded;
        fixture.files = [&](const Request &request) { if (request.method == "POST") { ++posts; uploaded = request.rawBody; } return Response{QJsonObject{}, 503}; };
        auto *clipboard = QGuiApplication::clipboard(); auto *old = new QMimeData;
        if (const auto *previous = clipboard->mimeData()) for (const auto &format : previous->formats()) old->setData(format, previous->data(format));
        const auto restore = qScopeGuard([&] { clipboard->setMimeData(old); });
        QVERIFY(fixture.start()); auto *data = new QMimeData; data->setUrls({QUrl::fromLocalFile(selected)}); clipboard->setMimeData(data);
        QVERIFY(fixture.controller.prepareClipboardFiles()); QTRY_VERIFY(!fixture.controller.clipboardFilesPreparing());
        const auto preview = fixture.controller.clipboardFilesPreview(); QVERIFY(!preview.isEmpty());
        fixture.controller.attachClipboardFiles(); QTRY_VERIFY(!fixture.controller.clipboardFilesPreparing());
        QVERIFY(QFileInfo(selected).isSymLink()); // the synthetic replacement actually happened
        if (!fixture.controller.error().isEmpty()) { QCOMPARE(posts, 0); return; }
        QTRY_COMPARE(posts, 1);
        const auto displayedTarget = preview.first().toMap().value("target").toString();
        QVERIFY2(!uploaded.contains("private-synthetic") || displayedTarget == secret,
            "The uploaded bytes belong to a file that was not shown for confirmation");
    }

    void clipboardSnapshotRejectsOversizedSecondFileAndCleansUp()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile first(directory.filePath("first.txt")), large(directory.filePath("too-large.txt"));
        QVERIFY(first.open(QIODevice::WriteOnly)); first.write("safe synthetic"); first.close();
        QVERIFY(large.open(QIODevice::WriteOnly)); QVERIFY(large.resize(25 * 1024 * 1024 + 1)); large.close();
        int posts = 0; fixture.files = [&](const Request &) { ++posts; return Response{QJsonObject{}, 503}; };
        auto *clipboard = QGuiApplication::clipboard(); auto *old = new QMimeData;
        if (const auto *previous = clipboard->mimeData()) for (const auto &format : previous->formats()) old->setData(format, previous->data(format));
        const auto restore = qScopeGuard([&] { clipboard->setMimeData(old); });
        QVERIFY(fixture.start());
        auto *data = new QMimeData; data->setUrls({QUrl::fromLocalFile(first.fileName()), QUrl::fromLocalFile(large.fileName())}); clipboard->setMimeData(data);
        QVERIFY(fixture.controller.prepareClipboardFiles());
        QTRY_VERIFY(!fixture.controller.clipboardFilesPreparing());
        QVERIFY(fixture.controller.clipboardFilesPreview().isEmpty()); QVERIFY(!fixture.controller.error().isEmpty());
        fixture.controller.attachClipboardFiles(); QTest::qWait(100); QCOMPARE(posts, 0);
        QVERIFY(fixture.controller.uploadEntries().isEmpty());
    }

    void clipboardSnapshotReadErrorAfterFirstFileCleansUp()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile first(directory.filePath("first.txt")), denied(directory.filePath("denied.txt"));
        QVERIFY(first.open(QIODevice::WriteOnly)); first.write("safe synthetic"); first.close();
        QVERIFY(denied.open(QIODevice::WriteOnly)); denied.write("synthetic"); denied.close();
        QVERIFY(denied.setPermissions({}));
        QFile verify(denied.fileName()); if (verify.open(QIODevice::ReadOnly)) QSKIP("Filesystem does not enforce file permissions for this user");
        auto *clipboard = QGuiApplication::clipboard(); auto *old = new QMimeData;
        if (const auto *previous = clipboard->mimeData()) for (const auto &format : previous->formats()) old->setData(format, previous->data(format));
        const auto restore = qScopeGuard([&] { denied.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); clipboard->setMimeData(old); });
        QVERIFY(fixture.start());
#ifdef Q_OS_LINUX
        const auto openAnonymousTemps = [] {
            int count = 0; const QDir descriptors("/proc/self/fd");
            for (const auto &fd : descriptors.entryList(QDir::System | QDir::NoDotAndDotDot)) {
                const auto path = QFile::encodeName(descriptors.filePath(fd)); char target[4096];
                const auto length = ::readlink(path.constData(), target, sizeof(target));
                if (length <= 0) continue;
                const QByteArray resolved(target, length);
                if (resolved.startsWith(QFile::encodeName(QDir::tempPath()) + "/#") && resolved.endsWith(" (deleted)")) ++count;
            }
            return count;
        };
        const auto initialTempFds = openAnonymousTemps();
#endif
        auto *data = new QMimeData; data->setUrls({QUrl::fromLocalFile(first.fileName()), QUrl::fromLocalFile(denied.fileName())}); clipboard->setMimeData(data);
        QVERIFY(fixture.controller.prepareClipboardFiles());
        QTRY_VERIFY(!fixture.controller.clipboardFilesPreparing());
        QVERIFY(!fixture.controller.clipboardFilesPreview().isEmpty());
        fixture.controller.attachClipboardFiles(); QTRY_VERIFY(!fixture.controller.clipboardFilesPreparing());
        QVERIFY(!fixture.controller.error().isEmpty()); QVERIFY(fixture.controller.uploadEntries().isEmpty());
#ifdef Q_OS_LINUX
        QTRY_COMPARE(openAnonymousTemps(), initialTempFds);
#endif
    }

    void clipboardSnapshotBudgetChecksCountAndTotal()
    {
        ClipboardSnapshotBudget budget(3, 10);
        QVERIFY(budget.add(4)); QVERIFY(budget.add(6));
        QVERIFY(!budget.add(1)); // total must not exceed the budget
        QVERIFY(budget.add(0)); // rejected additions did not consume a slot
        QVERIFY(!budget.add(0)); // file count is bounded independently
        ClipboardSnapshotBudget invalid(2, 10); QVERIFY(!invalid.add(-1));
    }

    void clipboardSnapshotRejectsTooManyFilesBeforeCopying()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("tiny.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("x"); input.close();
        auto *clipboard = QGuiApplication::clipboard(); auto *old = new QMimeData;
        if (const auto *previous = clipboard->mimeData()) for (const auto &format : previous->formats()) old->setData(format, previous->data(format));
        const auto restore = qScopeGuard([&] { clipboard->setMimeData(old); });
        QVERIFY(fixture.start()); QList<QUrl> urls;
        for (int i = 0; i < 101; ++i) urls.append(QUrl::fromLocalFile(input.fileName()));
        auto *data = new QMimeData; data->setUrls(urls); clipboard->setMimeData(data);
        fixture.controller.prepareClipboardFiles();
        QTRY_VERIFY(!fixture.controller.clipboardFilesPreparing());
        QVERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(fixture.controller.clipboardFilesPreview().isEmpty());
        fixture.controller.attachClipboardFiles(); QVERIFY(fixture.controller.uploadEntries().isEmpty());
    }

    void oversizedEditDoesNotProtectAnUnsubmittedUpload()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("report"); input.close(); int puts = 0, deletes = 0;
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") { ++deletes; return Response{QJsonObject{}, 204}; }
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 6},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201};
        };
        const auto note = message("Original", messageId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") ++puts;
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{note}} : Response{note};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(messageId));
        fixture.controller.setDraft(QString(40000, QLatin1Char('a')));
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())}); QTRY_VERIFY(!fixture.controller.fileBusy());
        const auto card = fixture.controller.uploadEntries().first().toMap().value("id").toString();
        fixture.controller.submitEditMessage(); QTest::qWait(100); QCOMPARE(puts, 0);
        fixture.controller.removeUpload(card); QTRY_COMPARE_WITH_TIMEOUT(deletes, 1, 2000);
        QCOMPARE(fixture.controller.draft().size(), 40000);
    }

    void uploadedAttachmentOrderInOutgoingBody_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("expected");
        const auto quote = QStringLiteral("[Sam](urn:quote:") + messageId + ")";
        QTest::newRow("plain") << QStringLiteral("Caption") << QStringLiteral("LINK\nCaption");
        QTest::newRow("existing") << QStringLiteral("Caption\nLINK") << QStringLiteral("Caption\nLINK");
        QTest::newRow("linked-in-list") << QStringLiteral("- LINK") << QStringLiteral("- LINK");
        QTest::newRow("code-span") << QStringLiteral("Example: `LINK`") << QStringLiteral("LINK\nExample: `LINK`");
        QTest::newRow("escaped-markup") << QStringLiteral("Example: \\LINK") << QStringLiteral("LINK\nExample: \\LINK");
        QTest::newRow("quote-and-answer") << quote + "\n\nCaption" << quote + "\n\nLINK\n\nCaption";
        QTest::newRow("quote-only") << quote << quote + "\n\nLINK";
        QTest::newRow("continued-paragraph") << quote + "\nanswer" << "LINK\n" + quote + "\nanswer";
        QTest::newRow("list-starts-new-block") << quote + "\n- list item" << quote + "\n\nLINK\n\n- list item";
        QTest::newRow("heading-starts-new-block") << quote + "\n# Heading" << quote + "\n\nLINK\n\n# Heading";
        QTest::newRow("setext-title") << quote + "\n===" << "LINK\n" + quote + "\n===";
        QTest::newRow("inline-quote") << quote + " text" << "LINK\n" + quote + " text";
    }

    void uploadedAttachmentOrderInOutgoingBody()
    {
        QFETCH(QString, body);
        QFETCH(QString, expected);
        Fixture f;
        FileClient files(&f.api);
        f.controller.useFiles(&files);
        QTemporaryDir directory;
        QFile file(directory.filePath("report.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("report"), 6);
        file.close();
        f.files = [](const Request &) { return Response{QJsonObject{
            {"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
            {"content_type", "text/plain"}, {"size_bytes", 6},
            {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201}; };
        QString sent;
        f.messages = [&](const Request &r) {
            if (r.method == "POST") {
                sent = r.body.value("payload").toObject().value("content").toString();
                return Response{QJsonObject{}, 503};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(f.start());
        f.controller.attachFiles({QUrl::fromLocalFile(file.fileName())});
        QTRY_VERIFY(!f.controller.fileBusy());
        QCOMPARE(f.controller.uploadEntries().size(), 1);
        const auto link = f.controller.uploadEntries().first().toMap().value("markdown").toString();
        QVERIFY(!link.isEmpty());
        f.controller.setDraft(body.replace("LINK", link));
        f.controller.send();
        QTRY_VERIFY(!sent.isEmpty());
        QCOMPARE(sent, expected.replace("LINK", link));
    }

    void acceptedAttachmentWireSurvivesUncertainSendAndNavigation()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useDrafts(&drafts);
        FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QFile input(directory.filePath("report.txt")); QVERIFY(input.open(QIODevice::WriteOnly)); input.write("report"); input.close();
        int deletes = 0, posts = 0; QString wire;
        fixture.files = [&](const Request &request) {
            if (request.method == "DELETE") { ++deletes; return Response{QJsonObject{}, 204}; }
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 6},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("report", QCryptographicHash::Sha256).toHex())}}, 201};
        };
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { ++posts; wire = request.body.value("payload").toObject().value("content").toString(); return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Caption"); fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy()); QCOMPARE(fixture.controller.draft(), QString("Caption"));
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send(); QTRY_COMPARE(posts, 1);
        QVERIFY(wire.startsWith("[report.txt]")); QVERIFY(wire.contains("urn:file:" + returnedId)); QVERIFY(wire.endsWith("Caption"));
        QTRY_VERIFY(!fixture.controller.busy()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(deletes, 0);
        const auto owner = drafts.owner(); bool flushed = false; drafts.flush(this, [&](bool ok) { flushed = ok; }); QTRY_VERIFY(flushed);
        storage.shutdown(); QTRY_VERIFY(storage.stopped());
        NativeDraftStore store(directory.filePath("drafts")); QCOMPARE(store.open(owner), NativeDraftService::Failure::None);
        const auto pending = store.outgoing(); QCOMPARE(pending.failure, NativeDraftService::Failure::None); QCOMPARE(pending.rows.size(), 1);
        QCOMPARE(pending.rows.first().payload.value("content").toString(), wire);
    }

    void attachmentUploadKeepsItsConversationAndEscapesMarkdown()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture;
        fixture.controller.useDrafts(&drafts);
        fixture.extraTopic = true;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("Hello attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report[one](two).txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(bytes), bytes.size());
        input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", input.fileName().section('/', -1)}, {"content_type", "text/plain"},
                {"size_bytes", bytes.size()}, {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 200, 200, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Initial text");
        QVERIFY(fixture.controller.canAttach());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy());
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.selectTopic(secondTopicId);
        fixture.controller.setDraft("Second draft");
        QTRY_VERIFY(fixture.controller.canSend());
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.draft(), "Second draft");
        fixture.controller.selectTopic(topicId);
        QCOMPARE(fixture.controller.draft(), QString("Initial text"));
        QVERIFY(fixture.controller.uploadEntries().isEmpty());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        QString wire;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") { wire = request.body.value("payload").toObject().value("content").toString(); return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{}};
        };
        fixture.controller.send(); QTRY_VERIFY(!wire.isEmpty());
        QVERIFY(wire.startsWith("[report\\[one\\](two).txt](<urn:file:"));
        NativeMarkdown markdown;
        const auto rendered = markdown.render(wire);
        QVERIFY(rendered.value("html").toString().contains("urn:file:" + returnedId));
        QVERIFY(rendered.value("plain").toString().contains("report[one](two).txt"));
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        draftStorage.shutdown(); QTRY_VERIFY(draftStorage.stopped());
        NativeDraftStore store(draftDirectory.path()); QCOMPARE(store.open(drafts.owner()), NativeDraftService::Failure::None);
        QCOMPARE(store.latest(streamId, topicId).record->text, QString("Initial text"));
    }

    void lateOrdinaryUploadDoesNotReplaceAnEncryptedMemoryDraft()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QFile input(directory.filePath("attachment.txt")); QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        std::function<void()> complete;
        const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "attachment.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Ordinary input"); fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(bool(complete)); fixture.encrypted = true; fixture.controller.refresh();
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(fixture.controller.streams().first().toMap().value("encrypted").toBool()
            || fixture.controller.streams().first().toMap().value("encryption").toBool());
        fixture.controller.setDraft("Encrypted memory input"); complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.draft(), QString("Encrypted memory input"));
        QVERIFY(fixture.controller.uploadEntries().isEmpty()); // encrypted send cannot carry an ordinary upload
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        storage.shutdown(); QTRY_VERIFY(storage.stopped());
        NativeDraftStore store(directory.filePath("drafts")); QCOMPARE(store.open(drafts.owner()), NativeDraftService::Failure::None);
        const auto prior = store.latest(streamId, topicId); QVERIFY(prior.record);
        QCOMPARE(prior.record->text, "Ordinary input");
    }

    void logoutClearsPendingAttachmentUiAndLateDrafts()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("Hello attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 200, 350, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy());
        fixture.controller.logout();
        QVERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.notice().isEmpty());
        QTest::qWait(400);
        QVERIFY(fixture.controller.draft().isEmpty());
        QVERIFY(!fixture.controller.canAttach());
    }

    void failedAttachmentRetainsAnActionableDownloadRow()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        fixture.files = [](const Request &) { return Response{QJsonObject{}, 503}; };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("report.txt")));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.error().contains("could not be saved"));
        const auto rows = fixture.controller.property("downloadEntries").toList();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("fileUuid").toString(), returnedId);
        QCOMPARE(rows.first().toMap().value("fileName").toString(), QString("report.txt"));
        QCOMPARE(rows.first().toMap().value("status").toString(), QString("error"));
    }

    void downloadRowsTrackProgressCancelDuplicatesAndUniqueDestinations_data()
    {
        QTest::addColumn<bool>("cancelSecond"); QTest::addColumn<bool>("symbolic");
        QTest::newRow("cancel") << true << false;
        QTest::newRow("complete-both") << false << false;
#ifdef Q_OS_UNIX
        QTest::newRow("dangling-symbolic-link") << false << true;
#endif
    }

    void downloadRowsTrackProgressCancelDuplicatesAndUniqueDestinations()
    {
        QFETCH(bool, cancelSecond); QFETCH(bool, symbolic);
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        const auto destination = directory.filePath("saved.txt");
        QFile original(destination);
        if (symbolic) {
#ifdef Q_OS_UNIX
            QCOMPARE(::symlink(QFile::encodeName(directory.filePath("missing-target.txt")).constData(), QFile::encodeName(destination).constData()), 0);
#endif
        } else { QVERIFY(original.open(QIODevice::WriteOnly)); original.write("keep me"); original.close(); }
        QVector<std::function<void()>> release;
        auto cleanup = qScopeGuard([&] { for (auto &send : release) if (send) send(); });
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response response{QJsonObject{}};
                response.defer = [&](auto send) { release.append(std::move(send)); };
                return response;
            }
            return Response{QJsonObject{{"uuid", request.target.path().section('/', -1)}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 2}}};
        };
        QVERIFY(fixture.start());
        const auto rows = [&] { return fixture.controller.downloadEntries(); };
        QSignalSpy duplicate(&fixture.controller, &NativeController::downloadDuplicateRequested);
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(destination));
        QCOMPARE(rows().size(), 1);
        QCOMPARE(rows().first().toMap().value("status").toString(), QString("starting"));
        const auto first = rows().first().toMap().value("id").toString();
        fixture.controller.downloadFile(returnedId.toUpper(), QUrl::fromLocalFile(destination));
        QCOMPARE(duplicate.size(), 1); QCOMPARE(rows().size(), 1);
        fixture.controller.retryDownload(first); // Active entries are not retryable.
        fixture.controller.dismissDownload(first); fixture.controller.clearDownloads();
        QCOMPARE(rows().size(), 1);
        QTRY_COMPARE(release.size(), 1);
        QCOMPARE(rows().first().toMap().value("status").toString(), QString("downloading"));
        QCOMPARE(rows().first().toMap().value("receivedBytes").toLongLong(), 0);
        QCOMPARE(rows().first().toMap().value("totalBytes").toLongLong(), 2);
        fixture.controller.downloadFile(messageId, QUrl::fromLocalFile(destination));
        QTRY_COMPARE(release.size(), 2);
        QCOMPARE(rows().size(), 2);
        const auto second = rows().first().toMap().value("id").toString();
        if (cancelSecond) fixture.controller.cancelDownload(second);
        QCOMPARE(rows().size(), cancelSecond ? 1 : 2);
        QVERIFY(fixture.controller.fileBusy());
        release[1](); release[1] = {}; release[0](); release[0] = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(rows().first().toMap().value("status").toString(), QString("downloaded"));
        QCOMPARE(rows().first().toMap().value("receivedBytes").toLongLong(), 2);
        QFile saved(directory.filePath("saved (1).txt")); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), QByteArray("{}"));
        QCOMPARE(QFileInfo::exists(directory.filePath("saved (2).txt")), !cancelSecond);
        if (symbolic) {
            QVERIFY(QFileInfo(destination).isSymLink());
            QVERIFY(!QFileInfo::exists(directory.filePath("missing-target.txt")));
        } else { QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("keep me")); }
        fixture.controller.retryDownload(first); QCOMPARE(rows().size(), cancelSecond ? 1 : 2);
        fixture.controller.clearDownloads(); QVERIFY(rows().isEmpty());
        QVERIFY(QFileInfo::exists(directory.filePath("saved (1).txt")));
        QVERIFY(QFileInfo::exists(destination) || QFileInfo(destination).isSymLink());
    }

    void downloadCommitPreservesAFileCreatedDuringTransfer_data()
    {
        QTest::addColumn<bool>("symbolic");
        QTest::newRow("regular-file") << false;
#ifdef Q_OS_UNIX
        QTest::newRow("symbolic-link") << true;
#endif
    }

    void downloadCommitPreservesAFileCreatedDuringTransfer()
    {
        QFETCH(bool, symbolic);
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        const auto destination = directory.filePath("report.txt");
        std::function<void()> release;
        auto cleanup = qScopeGuard([&] { if (release) release(); });
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response response{QJsonObject{}};
                response.defer = [&](auto send) { release = std::move(send); };
                return response;
            }
            return Response{QJsonObject{{"uuid", returnedId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 2}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(destination));
        QTRY_VERIFY(bool(release));
        if (symbolic) {
#ifdef Q_OS_UNIX
            QCOMPARE(::symlink(QFile::encodeName(directory.filePath("missing.txt")).constData(),
                QFile::encodeName(destination).constData()), 0);
#endif
        } else {
            QFile other(destination); QVERIFY(other.open(QIODevice::WriteOnly));
            QCOMPARE(other.write("other application"), 17); other.close();
        }
        release(); release = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        if (symbolic) {
            QVERIFY(QFileInfo(destination).isSymLink());
            QVERIFY(!QFileInfo::exists(directory.filePath("missing.txt")));
        } else {
            QFile other(destination); QVERIFY(other.open(QIODevice::ReadOnly));
            QCOMPARE(other.readAll(), QByteArray("other application"));
        }
        QCOMPARE(fixture.controller.downloadEntries().first().toMap().value("status").toString(), QString("error"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).size(), 1);
    }

    void downloadRetryRecoversAfterDestinationAllocationFailure()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        const auto destination = directory.filePath("report.txt");
        QVERIFY(QDir().mkdir(destination));
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) return Response{QJsonObject{}};
            return Response{QJsonObject{{"uuid", returnedId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 2}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(destination));
        QCOMPARE(fixture.controller.downloadEntries().size(), 1);
        const auto row = fixture.controller.downloadEntries().first().toMap();
        QCOMPARE(row.value("status").toString(), QString("error"));
        QVERIFY(QDir().rmdir(destination));
        fixture.controller.retryDownload(row.value("id").toString());
        QTRY_VERIFY(fixture.controller.downloadEntries().first().toMap().value("id") != row.value("id"));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.downloadEntries().first().toMap().value("status").toString(), QString("downloaded"));
        QFile saved(destination); QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("{}"));
    }

    void downloadRetryWaitsForFreshSessionAndRetainsFailures_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"changed-token", "same-token", "refresh-fails", "restart-fails", "removed", "logout", "service-replaced"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void downloadRetryWaitsForFreshSessionAndRetainsFailures()
    {
        QFETCH(QString, scenario);
        Fixture fixture;
        FileClient files(&fixture.api), replacement(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        fixture.files = [](const Request &) { return Response{QJsonObject{}, 503}; };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("report.txt")));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        const auto rows = [&] { return fixture.controller.downloadEntries(); };
        QCOMPARE(rows().size(), 1);
        const auto original = rows().first().toMap();
        const auto oldId = original.value("id");
        std::function<void()> refresh, metadata;
        auto cleanup = qScopeGuard([&] { if (refresh) refresh(); if (metadata) metadata(); });
        int refreshes = 0, starts = 0;
        QByteArray bearer;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/get_token/invoke")) {
                ++refreshes;
                if (scenario == "refresh-fails") response.status = 503;
                else if (scenario != "same-token") {
                    auto body = response.data.toObject(); body.insert("access_token", "synthetic-refreshed-token"); response.data = body;
                }
                response.defer = [&](auto send) { refresh = std::move(send); };
            }
            return response;
        };
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) return Response{QJsonObject{}};
            ++starts; bearer = request.headers.value("authorization");
            Response response{QJsonObject{{"uuid", returnedId}, {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 2}}};
            if (scenario == "restart-fails") response.status = 503;
            response.defer = [&](auto send) { metadata = std::move(send); };
            return response;
        };
        fixture.controller.retryDownload(oldId.toString());
        fixture.controller.retryDownload(oldId.toString());
        QTRY_VERIFY(bool(refresh));
        QCOMPARE(refreshes, 1); QCOMPARE(starts, 0);
        QCOMPARE(rows().first().toMap().value("id"), oldId);
        QVERIFY(rows().first().toMap().value("retrying").toBool());
        if (scenario == "removed") fixture.controller.dismissDownload(oldId.toString());
        if (scenario == "logout") fixture.controller.logout();
        if (scenario == "service-replaced") fixture.controller.useFiles(&replacement);
        refresh(); refresh = {};
        if (scenario == "removed" || scenario == "logout") {
            QTest::qWait(150); QCOMPARE(starts, 0); QVERIFY(rows().isEmpty()); return;
        }
        if (scenario == "refresh-fails" || scenario == "service-replaced") {
            QTRY_VERIFY(!rows().first().toMap().value("retrying").toBool());
            QCOMPARE(rows().size(), 1); QCOMPARE(rows().first().toMap().value("id"), oldId);
            QCOMPARE(rows().first().toMap().value("status").toString(), QString("error"));
            QVERIFY(!fixture.controller.error().isEmpty()); QCOMPARE(starts, 0); return;
        }
        QTRY_VERIFY(bool(metadata));
        QCOMPARE(starts, 1); QCOMPARE(rows().size(), 1);
        const auto retry = rows().first().toMap();
        QVERIFY(retry.value("id") != oldId);
        QCOMPARE(retry.value("fileName"), original.value("fileName"));
        QCOMPARE(retry.value("fileUuid"), original.value("fileUuid"));
        QCOMPARE(retry.value("status").toString(), QString("starting"));
        QCOMPARE(bearer, scenario == "same-token" ? QByteArray("Bearer synthetic-project-token") : QByteArray("Bearer synthetic-refreshed-token"));
        fixture.controller.retryDownload(retry.value("id").toString());
        QCOMPARE(refreshes, 1);
        metadata(); metadata = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(rows().size(), 1);
        QCOMPARE(rows().first().toMap().value("id"), retry.value("id"));
        QCOMPARE(rows().first().toMap().value("status").toString(), scenario == "restart-fails" ? QString("error") : QString("downloaded"));
    }

    void downloadRowsRetireTransfersAndNeverCrossOwners()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        std::function<void()> release;
        auto cleanup = qScopeGuard([&] { if (release) release(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{}, 503};
            response.defer = [&](auto send) { release = std::move(send); };
            return response;
        };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("report.txt")));
        QTRY_VERIFY(bool(release));
        fixture.api.setAccessToken("synthetic-replacement-token");
        QVERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.downloadEntries().first().toMap().value("status").toString(), QString("error"));
        release(); release = {};
        fixture.controller.logout();
        QVERIFY(fixture.controller.downloadEntries().isEmpty());
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.downloadEntries().isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath("report.txt")));
    }

    void downloadedFileActionsUseTheVerifiedLocalPath()
    {
#ifdef Q_OS_LINUX
        if (!qEnvironmentVariableIsSet("CASSI_NATIVE_DOWNLOAD_ACTION_CHILD")) {
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("CASSI_NATIVE_DOWNLOAD_ACTION_CHILD", "1");
            environment.insert("QT_QPA_PLATFORM", "offscreen");
            child.setProcessEnvironment(environment);
            child.start("dbus-run-session", {"--", QCoreApplication::applicationFilePath(),
                "downloadedFileActionsUseTheVerifiedLocalPath"});
            QVERIFY(child.waitForFinished(15000));
            const auto output = child.readAllStandardOutput() + child.readAllStandardError();
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QVERIFY2(child.exitCode() == 0, output.constData());
            return;
        }
#endif
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        const auto destination = directory.filePath("report with spaces.txt");
        fixture.files = [](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) return Response{QJsonObject{}};
            return Response{QJsonObject{{"uuid", returnedId}, {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 2}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(destination));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        const auto id = fixture.controller.downloadEntries().first().toMap().value("id").toString();
        DownloadDesktopProbe probe;
        QDesktopServices::setUrlHandler("file", &probe, "open");
        auto clearHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        fixture.controller.openDownload(id);
        QCOMPARE(probe.opened, QList<QUrl>{QUrl::fromLocalFile(destination)});
#ifdef Q_OS_LINUX
        auto bus = QDBusConnection::sessionBus();
        QVERIFY2(bus.registerService("org.freedesktop.FileManager1"), "Run this test under dbus-run-session; never replace the desktop file manager");
        auto clearBus = qScopeGuard([&] { bus.unregisterObject("/org/freedesktop/FileManager1"); bus.unregisterService("org.freedesktop.FileManager1"); });
        QVERIFY(bus.registerObject("/org/freedesktop/FileManager1", &probe, QDBusConnection::ExportAllSlots));
        fixture.controller.revealDownload(id);
        QTRY_COMPARE(probe.revealed, QStringList{QUrl::fromLocalFile(destination).toString(QUrl::FullyEncoded)});
#endif
        QVERIFY(QFile::remove(destination));
        fixture.controller.openDownload(id);
        QCOMPARE(probe.opened.size(), 1);
        QCOMPARE(fixture.controller.downloadEntries().first().toMap().value("status").toString(), QString("error"));
        QVERIFY(fixture.controller.error().contains("missing"));
    }

    void attachmentDownloadUsesVerifiedFileClientAndLocalDestination()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        int fileRequests = 0;
        fixture.files = [&](const Request &request) {
            ++fileRequests;
            if (request.target.path().endsWith("/actions/download"))
                return Response{QJsonObject{}}; // The mock server serializes exactly two bytes: {}.
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "download.bin"},
                {"content_type", "application/octet-stream"}, {"size_bytes", 2},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("{}", QCryptographicHash::Sha256).toHex())}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl("https://example.invalid/destination"));
        QCOMPARE(fileRequests, 0);
        const auto destination = directory.filePath("saved.bin");
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(destination));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.error().isEmpty());
        QCOMPARE(fileRequests, 2);
        QFile output(destination);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), "{}");
        QVERIFY(fixture.controller.notice().contains("saved.bin"));
    }

    void attachmentDownloadProjectionFollowsTheFileUuidReference()
    {
        // Issue #631 port of findAttachmentDownload + describeDownload
        // (workspace_ui 6b9f27f workspace-message-attachment-download.lib.ts:12-24,
        // workspace-message-attachment-download.ui.tsx:12-27).
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        QVector<std::function<void()>> release;
        auto cleanup = qScopeGuard([&] { for (auto &send : release) if (send) send(); });
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response response{QJsonObject{}};
                response.defer = [&](auto send) { release.append(std::move(send)); };
                return response;
            }
            // file-client.cpp:76-78 validates size_bytes as required metadata,
            // so an unknown total only occurs while a fresh entry is starting.
            return Response{QJsonObject{{"uuid", request.target.path().section('/', -1)},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 2}}};
        };
        QVERIFY(fixture.start());
        const auto state = [&](const QString &uuid) {
            QVariantMap result;
            QMetaObject::invokeMethod(&fixture.controller, "attachmentDownload",
                Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, uuid));
            return result;
        };
        QVERIFY(state(returnedId).isEmpty()); // No entry: the card slot stays unmounted.
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("report.txt")));
        QCOMPARE(state(returnedId).value("status").toString(), QString("starting"));
        QCOMPARE(state(returnedId).value("percent").toInt(), -1); // Percent is unknown until bytes can flow.
        QTRY_COMPARE(release.size(), 1);
        QCOMPARE(state(returnedId).value("status").toString(), QString("downloading"));
        QCOMPARE(state(returnedId).value("percent").toInt(), 0); // Total known, no bytes yet: 0 %.
        release[0](); release[0] = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(state(returnedId).value("status").toString(), QString("downloaded"));
        QCOMPARE(state(returnedId).value("percent").toInt(), -1);
        // An active retry wins over the completed entry for the same fileUuid.
        const auto firstId = state(returnedId).value("id").toString();
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("again.txt")));
        QCOMPARE(state(returnedId).value("status").toString(), QString("starting"));
        QVERIFY(state(returnedId).value("id").toString() != firstId); // The active entry, not the terminal one.
        QCOMPARE(state(returnedId).value("percent").toInt(), -1); // starting keeps the reference's null percent.
        QTRY_COMPARE(release.size(), 2);
        QCOMPARE(state(returnedId).value("status").toString(), QString("downloading"));
        QCOMPARE(state(returnedId).value("percent").toInt(), 0);
        release[1](); release[1] = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(state(returnedId).value("status").toString(), QString("downloaded"));
        // Failed entries stay projected so the card can offer Retry.
        fixture.files = [](const Request &) { return Response{QJsonObject{}, 503}; };
        fixture.controller.downloadFile(messageId, QUrl::fromLocalFile(directory.filePath("failed.txt")));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(state(messageId).value("status").toString(), QString("error"));
        QVERIFY(state(QUuid::createUuid().toString(QUuid::WithoutBraces)).isEmpty());
        fixture.controller.clearDownloads();
        QVERIFY(state(returnedId).isEmpty());
    }

    void attachmentDownloadReportsStableCompletionTime()
    {
        // Review finding 4650261678 (issue #631): the downloaded state carries
        // completedAtMs - the wall-clock instant the transfer finished - so a
        // recreated card resumes its confirmation window from the real
        // completion instead of restarting the full window.
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        QVector<std::function<void()>> release;
        auto cleanup = qScopeGuard([&] { for (auto &send : release) if (send) send(); });
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response response{QJsonObject{}};
                response.defer = [&](auto send) { release.append(std::move(send)); };
                return response;
            }
            return Response{QJsonObject{{"uuid", request.target.path().section('/', -1)},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 2}}};
        };
        QVERIFY(fixture.start());
        const auto state = [&](const QString &uuid) {
            QVariantMap result;
            QMetaObject::invokeMethod(&fixture.controller, "attachmentDownload",
                Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, uuid));
            return result;
        };
        const auto before = QDateTime::currentMSecsSinceEpoch();
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("report.txt")));
        QTRY_COMPARE(release.size(), 1);
        QCOMPARE(state(returnedId).value("completedAtMs").toLongLong(), 0LL); // Not finished yet.
        release[0](); release[0] = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        const auto finished = state(returnedId);
        QCOMPARE(finished.value("status").toString(), QString("downloaded"));
        const auto completedAtMs = finished.value("completedAtMs").toLongLong();
        QVERIFY(completedAtMs >= before); // Recorded at the completion instant, not at read time.
        QThread::msleep(30);
        QCOMPARE(state(returnedId).value("completedAtMs").toLongLong(), completedAtMs); // Stable across reads.
    }

    void encryptedConversationNeverUploadsPlaintextAttachment()
    {
        Fixture fixture;
        fixture.encrypted = true;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        int uploads = 0;
        fixture.files = [&](const Request &) { ++uploads; return Response{}; };
        QVERIFY(fixture.start());
        QVERIFY(!fixture.controller.canAttach());
        fixture.controller.attachFiles({QUrl::fromLocalFile("/tmp/irrelevant.txt")});
        QCOMPARE(uploads, 0);
        QVERIFY(!fixture.controller.fileBusy());
    }

    void sessionRenewalOnlyCancelsTransfersWhenCredentialGenerationChanges_data()
    {
        QTest::addColumn<bool>("rotate");
        QTest::newRow("same-token") << false;
        QTest::newRow("rotated-token") << true;
    }

    void sessionRenewalOnlyCancelsTransfersWhenCredentialGenerationChanges()
    {
        QFETCH(bool, rotate);
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        int refreshes = 0;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (rotate && request.body.value("grant_type") == "refresh_token") {
                auto token = response.data.toObject();
                token.insert("access_token", QString("synthetic-rotated-%1").arg(++refreshes));
                response.data = token;
            }
            return response;
        };
        const QByteArray bytes("Hello attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 200, 450, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Keep my text");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy());
        QSignalSpy renewed(&fixture.session, &Session::authenticated);
        fixture.session.requestRefresh();
        QTRY_COMPARE(renewed.size(), 1);
        if (!rotate)
            QVERIFY(fixture.controller.fileBusy());
        QTRY_VERIFY(!fixture.controller.fileBusy());
        if (rotate) {
            QCOMPARE(fixture.controller.draft(), "Keep my text");
            QVERIFY(fixture.controller.notice().contains("interrupted"));
            QTest::qWait(500);
            QCOMPARE(fixture.controller.draft(), "Keep my text");
        } else {
            // Upload-card contract: an unfinished transfer survives the same-token renewal and its card
            // reports ready while the user's text stays untouched in the visible draft; the urn:file
            // reference rides on the card and enters the outgoing body at send.
            const auto cards = fixture.controller.uploadEntries();
            QCOMPARE(cards.size(), 1);
            QCOMPARE(cards.first().toMap().value("status").toString(), QString("ready"));
            QVERIFY(cards.first().toMap().value("markdown").toString().contains("urn:file:"));
            QCOMPARE(fixture.controller.draft(), QString("Keep my text"));
        }
    }
};

QTEST_MAIN(ControllerTransfersTest)
#include "native-controller-transfers-test.moc"
