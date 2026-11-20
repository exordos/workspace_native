#include "native-markdown.h"
#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-mention-fixtures.h"

class ControllerRepliesTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void inlineEditAttachmentStaysWithTheEditedMessage_data()
    {
        QTest::addColumn<bool>("durable");
        QTest::newRow("memory-draft") << false;
        QTest::newRow("durable-draft") << true;
    }

    void inlineEditAttachmentStaysWithTheEditedMessage()
    {
        QFETCH(bool, durable);
        QTemporaryDir directory;
        NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture;
        if (durable) fixture.controller.useDrafts(&drafts);
        FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QFile input(directory.filePath("report[one].txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        std::function<void()> complete, completeSave;
        const auto release = qScopeGuard([&] { if (complete) complete(); if (completeSave) completeSave(); });
        int uploads = 0;
        fixture.files = [&](const Request &) {
            ++uploads;
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", "report[one].txt"}, {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}, 201};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        auto note = message("Original message");
        QString saved; int puts = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") {
                ++puts; saved = request.body.value("payload").toObject().value("content").toString();
                note["payload"] = request.body.value("payload");
                Response response{note}; response.defer = [&](auto send) { completeSave = std::move(send); };
                return response;
            }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{note}} : Response{note};
        };
        QVERIFY(fixture.start());
        if (durable) QTRY_VERIFY(drafts.ready(streamId, topicId));
        const QString originalDraft = "Unrelated draft\n  preserve whitespace  \n";
        fixture.controller.setDraft(originalDraft);
        QVERIFY(fixture.controller.beginEditMessage(messageId));
        QVERIFY(fixture.controller.canAttach());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(bool(complete));
        fixture.controller.setDraft("Text typed while uploading");
        complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY2(fixture.controller.error().isEmpty(), qPrintable(fixture.controller.error()));
        const QString expected = "[report\\[one\\].txt](<urn:file:" + returnedId + "?name=report%5Bone%5D.txt&content_type=text%2Fplain&size=7>)\nText typed while uploading";
        QCOMPARE(fixture.controller.draft(), QString("Text typed while uploading"));
        QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        if (durable) QCOMPARE(drafts.text(streamId, topicId), originalDraft);
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(puts, 1);
        QCOMPARE(saved, expected);
        QTRY_VERIFY(bool(completeSave));
        QVERIFY(!fixture.controller.canAttach());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(!fixture.controller.fileBusy());
        QCOMPARE(uploads, 1);
        completeSave(); completeSave = {};
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), originalDraft);
        if (durable) QCOMPARE(drafts.text(streamId, topicId), originalDraft);
    }

    void inlineEditSaveWaitsForPendingAttachment()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        std::function<void()> complete;
        const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{}, 503};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        int puts = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") ++puts;
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{message()}} : Response{message()};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Unrelated draft");
        QVERIFY(fixture.controller.beginEditMessage(messageId));
        fixture.controller.setDraft("Edited body");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(bool(complete));
        QVERIFY(!fixture.controller.canSaveEdit());
        fixture.controller.submitEditMessage();
        QCOMPARE(puts, 0);
        complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY(!fixture.controller.canSaveEdit());
        QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        QCOMPARE(fixture.controller.uploadEntries().first().toMap().value("status").toString(), QString("error"));
        QVERIFY(fixture.controller.error().contains("could not be uploaded"));
        fixture.controller.removeUpload(fixture.controller.uploadEntries().first().toMap().value("id").toString());
        QVERIFY(fixture.controller.canSaveEdit());
        QCOMPARE(fixture.controller.draft(), QString("Edited body"));
        fixture.controller.cancelEditMessage();
        QCOMPARE(fixture.controller.draft(), QString("Unrelated draft"));
    }

    void inlineEditAbandonedUploadsCannotReachAnotherComposer_data()
    {
        QTest::addColumn<QString>("transition");
        QTest::newRow("cancel") << QString("cancel");
        QTest::newRow("edit") << QString("edit");
        QTest::newRow("edit-switch") << QString("edit-switch");
        QTest::newRow("reopen-same-message") << QString("reopen");
        QTest::newRow("replace-edit") << QString("replace");
        QTest::newRow("change-topic") << QString("topic");
    }

    void inlineEditAbandonedUploadsCannotReachAnotherComposer()
    {
        QFETCH(QString, transition);
        Fixture fixture; fixture.extraTopic = true;
        FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        QList<std::function<void()>> completions; int uploads = 0;
        const auto release = qScopeGuard([&] { for (auto &complete : completions) complete(); });
        fixture.files = [&](const Request &) {
            ++uploads;
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}, 201};
            response.defer = [&](auto send) { completions.append(std::move(send)); }; return response;
        };
        fixture.messages = [](const Request &request) -> Response {
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{message()}} : Response{message()};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Unrelated draft");
        QVERIFY(fixture.controller.beginEditMessage(messageId));
        QVariantList selection;
        for (int i = 0; i < 7; ++i) selection.append(QUrl::fromLocalFile(input.fileName()));
        fixture.controller.attachFiles(selection);
        QTRY_COMPARE(completions.size(), 5); // two uploads are still queued
        if (transition == "topic") {
            fixture.controller.selectTopic(secondTopicId);
            QTRY_VERIFY(!fixture.controller.busy());
        } else {
            if (transition != "replace") fixture.controller.cancelEditMessage();
            if (transition != "cancel") QVERIFY(fixture.controller.beginEditMessage(messageId));
        }
        QVERIFY(!fixture.controller.fileBusy());
        fixture.controller.setDraft("New composer text");
        for (auto &complete : completions) complete();
        completions.clear();
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(uploads, 5);
        QCOMPARE(fixture.controller.draft(), QString("New composer text"));
        QVERIFY(!fixture.controller.notice().contains("Uploading"));
        if (transition == "topic") {
            fixture.controller.selectTopic(topicId);
            QTRY_VERIFY(!fixture.controller.busy());
            QCOMPARE(fixture.controller.draft(), QString("Unrelated draft"));
        } else if (transition != "cancel") {
            fixture.controller.cancelEditMessage();
            QCOMPARE(fixture.controller.draft(), QString("Unrelated draft"));
        }
    }

    void inlineEditUploadKeepsItsRestoredReplyTab()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        const QString otherId = "7b7b7b7b-7b7b-4b7b-8b7b-7b7b7b7b7b7b";
        const auto first = message("First origin", messageId), second = message("Second origin", otherId);
        auto note = message(QString("[First](urn:quote:%1)\n\nfirst answer\n\n[Second](urn:quote:%2)\n\nsecond answer")
            .arg(messageId, otherId), returnedId);
        QString saved; int puts = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") {
                ++puts; saved = request.body.value("payload").toObject().value("content").toString();
                note["payload"] = request.body.value("payload"); return {note};
            }
            const auto path = request.target.path();
            if (path.endsWith("/messages/")) return {QJsonArray{first, second, note}};
            return {path.endsWith(messageId) ? first : path.endsWith(otherId) ? second : note};
        };
        std::function<void()> complete;
        const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}, 201};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Unrelated draft");
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto tabs = fixture.controller.replyTabs(); QCOMPARE(tabs.size(), 2);
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(bool(complete));
        QVERIFY(fixture.controller.selectReplyTab(tabs[1].toMap().value("id").toString()));
        fixture.controller.setDraft("Second answer typed while uploading");
        complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.draft(), QString("Second answer typed while uploading"));
        QVERIFY(fixture.controller.selectReplyTab(tabs[0].toMap().value("id").toString()));
        QCOMPARE(fixture.controller.draft(), QString("first answer"));
        QCOMPARE(fixture.controller.uploadEntries().size(), 1);
        fixture.controller.submitEditMessage(); QTRY_COMPARE(puts, 1);
        QCOMPARE(saved, "[report.txt](<urn:file:" + returnedId + "?name=report.txt&content_type=text%2Fplain&size=7>)\n[First](urn:quote:" + messageId
            + ")\n\nfirst answer\n\n[Second](urn:quote:"
            + otherId + ")\n\nSecond answer typed while uploading");
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("Unrelated draft"));
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto reopened = fixture.controller.replyTabs();
        QCOMPARE(reopened.size(), 2);
        QCOMPARE(fixture.controller.editAttachments().size(), 1);
        QCOMPARE(fixture.controller.draft(), QString("first answer"));
        QVERIFY(fixture.controller.selectReplyTab(reopened[1].toMap().value("id").toString()));
        QCOMPARE(fixture.controller.draft(), QString("Second answer typed while uploading"));
        QCOMPARE(fixture.controller.editAttachments().size(), 1);
        const auto originalWire = saved;
        fixture.controller.submitEditMessage(); QTRY_COMPARE(puts, 2);
        QCOMPARE(saved, originalWire);
    }

    void inlineEditCancellationKeepsAnEarlierOrdinaryUpload()
    {
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        QList<std::function<void()>> completions;
        const auto release = qScopeGuard([&] { for (auto &complete : completions) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}, 201};
            response.defer = [&](auto send) { completions.append(std::move(send)); }; return response;
        };
        fixture.messages = [](const Request &request) -> Response {
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{message()}} : Response{message()};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Ordinary draft");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_COMPARE(completions.size(), 1);
        QVERIFY(fixture.controller.beginEditMessage(messageId));
        QVERIFY(fixture.controller.canSaveEdit()); // entering edit discarded the ordinary upload
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_COMPARE(completions.size(), 2);
        QVERIFY(!fixture.controller.canSaveEdit()); // this edit now has its own pending upload
        fixture.controller.cancelEditMessage();
        QVERIFY(!fixture.controller.fileBusy()); // both composer sessions were discarded
        QCOMPARE(fixture.controller.draft(), QString("Ordinary draft"));
        completions[1](); completions[0](); completions.clear();
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.draft(), QString("Ordinary draft"));
        QVERIFY(fixture.controller.uploadEntries().isEmpty());
    }

    void inlineEditExtractsExistingAttachmentsBeforeEditing()
    {
        const QString file = "[report.txt](<urn:file:99999999-9999-4999-8999-999999999999?name=report.txt&content_type=text%2Fplain&size=7>)";
        const QString photo = "![photo.png](<urn:image:99999999-9999-4999-8999-999999999998?name=photo.png&content_type=image%2Fpng&size=7>)";
        Fixture fixture;
        const auto row = message(file + "\n" + photo + "\nA caption", returnedId);
        fixture.messages = [&](const Request &) -> Response { return {QJsonArray{row}}; };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.draft(), QString("A caption"));
        QCOMPARE(fixture.controller.property("editAttachments").toList().size(), 2);
    }

    void inlineEditAttachmentRemovalKeepsVerbatimLinesAndCode()
    {
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt&size=7>)";
        const QString image = "![photo](<urn:image:" + messageId + "?name=photo.png>)";
        const QString body = "Before " + image + " after\n\n```md\n" + image + "\n```";
        Fixture fixture; QString saved;
        auto row = message(file + "\n" + body, returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.draft(), body);
        const auto cards = fixture.controller.property("editAttachments").toList(); QCOMPARE(cards.size(), 2);
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller, "removeEditAttachment", Q_ARG(QString, cards[1].toMap()["id"].toString())));
        QCOMPARE(fixture.controller.draft(), "Before  after\n\n```md\n" + image + "\n```");
        QCOMPARE(fixture.controller.property("editAttachments").toList().size(), 1);
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, file + "\nBefore  after\n\n```md\n" + image + "\n```");
    }

    void inlineEditAttachmentOnlyAndSessionReset()
    {
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt>)";
        Fixture fixture; fixture.extraTopic = true; QString saved;
        auto row = message(file, returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Stored draft");
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QVERIFY(fixture.controller.draft().isEmpty()); QVERIFY(fixture.controller.canSaveEdit());
        fixture.controller.submitEditMessage(); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty()); QCOMPARE(saved, file);
        QVERIFY(fixture.controller.property("editAttachments").toList().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("Stored draft"));
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto old = fixture.controller.property("editAttachments").toList()[0].toMap()["id"].toString();
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller, "removeEditAttachment", Q_ARG(QString, old)));
        auto cards = fixture.controller.property("editAttachments").toList(); QCOMPARE(cards.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller, "removeEditAttachment", Q_ARG(QString, cards[0].toMap()["id"].toString())));
        QVERIFY(!fixture.controller.canSaveEdit());
        fixture.controller.cancelEditMessage(); QCOMPARE(fixture.controller.draft(), QString("Stored draft"));
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(fixture.controller.property("editAttachments").toList().isEmpty());
    }

    void inlineEditAttachmentRemovalDuringSaveRetainsTheNewRevision()
    {
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt>)";
        Fixture fixture; QString saved; std::function<void()> complete;
        const auto release = qScopeGuard([&] { if (complete) complete(); });
        auto row = message(file + "\nBody", returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") {
                saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"];
                Response response{row}; response.defer = [&](auto send) { complete = std::move(send); }; return response;
            }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto id = fixture.controller.property("editAttachments").toList().value(0).toMap()["id"].toString(); QVERIFY(!id.isEmpty());
        fixture.controller.submitEditMessage(); QTRY_VERIFY(bool(complete));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller, "removeEditAttachment", Q_ARG(QString, id)));
        complete(); complete = {}; QTRY_VERIFY(fixture.controller.canSaveEdit());
        QCOMPARE(fixture.controller.editingMessageId(), returnedId); QCOMPARE(fixture.controller.draft(), QString("Body"));
        QVERIFY(fixture.controller.property("editAttachments").toList().isEmpty());
        QCOMPARE(saved, file + "\nBody");
        fixture.controller.submitEditMessage(); QTRY_VERIFY(bool(complete));
        complete(); complete = {}; QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty()); QCOMPARE(saved, QString("Body"));
    }

    void inlineEditAttachmentsStayWithRestoredReplyAnswers()
    {
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt>)";
        const QString image = "![photo](<urn:image:" + messageId + "?name=photo.png>)";
        const QString secondId = "7b7b7b7b-7b7b-4b7b-8b7b-7b7b7b7b7b7b";
        const QString quote1 = "[One](urn:quote:" + messageId + ")", quote2 = "[Two](urn:quote:" + secondId + ")";
        Fixture fixture; QString saved;
        auto row = message(quote1 + "\n\n" + file + "\nFirst\n\n" + quote2 + "\n\n" + image + "\nSecond", returnedId);
        const auto one = message("Origin one", messageId), two = message("Origin two", secondId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            if (request.target.path().endsWith("/messages/")) return {QJsonArray{one, two, row}};
            return {request.target.path().endsWith(messageId) ? one : request.target.path().endsWith(secondId) ? two : row};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.draft(), QString("First"));
        const auto tabs = fixture.controller.replyTabs(); QCOMPARE(tabs.size(), 2);
        auto cards = fixture.controller.property("editAttachments").toList(); QCOMPARE(cards.size(), 1);
        QCOMPARE(cards[0].toMap()["markdown"].toString(), file);
        QVERIFY(fixture.controller.selectReplyTab(tabs[1].toMap()["id"].toString()));
        QCOMPARE(fixture.controller.draft(), QString("Second"));
        cards = fixture.controller.property("editAttachments").toList(); QCOMPARE(cards.size(), 1);
        QCOMPARE(cards[0].toMap()["markdown"].toString(), image);
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller, "removeEditAttachment", Q_ARG(QString, cards[0].toMap()["id"].toString())));
        fixture.controller.submitEditMessage(); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, quote1 + "\n\n" + file + "\nFirst\n\n" + quote2 + "\n\nSecond");
    }

    void inlineEditRetainedAndNewAttachmentsSerializeExactlyOnce()
    {
        const QString file = "[old.txt](<urn:file:" + messageId + "?name=old.txt>)";
        const QString photo = "![photo](<urn:image:" + returnedId + "?name=photo.png>)";
        const QString inlinePhoto = "![inline](<urn:image:" + secondTopicId + "?name=inline.png>)";
        const QString body = "Before " + inlinePhoto + " after";
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("new.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        auto row = message(file + "\n" + photo + "\n" + body, returnedId); QString saved;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        std::function<void()> complete;
        const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", "new.txt"}, {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}, 201};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.draft(), body);
        const auto cards = fixture.controller.property("editAttachments").toList(); QCOMPARE(cards.size(), 3);
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller, "removeEditAttachment", Q_ARG(QString, cards[0].toMap()["id"].toString())));
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())}); QTRY_VERIFY(bool(complete));
        QVERIFY(!fixture.controller.canSaveEdit());
        complete(); complete = {}; QTRY_VERIFY(!fixture.controller.fileBusy()); QVERIFY(fixture.controller.canSaveEdit());
        const auto uploaded = "[new.txt](<urn:file:" + returnedId + "?name=new.txt&content_type=text%2Fplain&size=7>)";
        fixture.controller.submitEditMessage(); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, uploaded + "\n" + photo + "\n" + body);
        QVERIFY(!saved.contains(file)); QCOMPARE(saved.count(inlinePhoto), 1); QCOMPARE(saved.count(uploaded), 1);
    }

    void inlineEditDismissingRestoredQuotesKeepsTheActiveAttachments_data()
    {
        QTest::addColumn<bool>("removeLast");
        QTest::newRow("cancel-reply") << false;
        QTest::newRow("remove-last-tab") << true;
    }
    void inlineEditDismissingRestoredQuotesKeepsTheActiveAttachments()
    {
        QFETCH(bool, removeLast);
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt>)";
        Fixture fixture; QString saved;
        auto row = message("[Origin](urn:quote:" + messageId + ")\n\n" + file + "\nAnswer", returnedId);
        const auto origin = message("Original", messageId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            if (request.target.path().endsWith("/messages/")) return {QJsonArray{origin, row}};
            return {request.target.path().endsWith(messageId) ? origin : row};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.draft(), QString("Answer"));
        if (removeLast) fixture.controller.removeReplyTab(fixture.controller.replyTabs()[0].toMap()["id"].toString());
        else fixture.controller.cancelReply();
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QCOMPARE(fixture.controller.property("editAttachments").toList().size(), 1);
        fixture.controller.submitEditMessage(); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, file + "\nAnswer");
    }

    void inlineEditReferenceStyleImageHasARemovableCard()
    {
        const QString urn = "urn:image:" + returnedId + "?name=photo.png";
        const QString body = "Before ![photo][asset] after\n\n[asset]: " + urn;
        NativeMarkdown parser;
        QCOMPARE(parser.mediaReferences(body).size(), 1); // existing renderer already recognises it
        Fixture fixture; QString saved; auto row = message(body, returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.draft(), body);
        const auto cards = fixture.controller.property("editAttachments").toList(); QCOMPARE(cards.size(), 1);
        QCOMPARE(cards[0].toMap()["reference"].toString(), urn);
        QCOMPARE(cards[0].toMap()["markdown"].toString(), QString("![photo][asset]"));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller, "removeEditAttachment", Q_ARG(QString, cards[0].toMap()["id"].toString())));
        fixture.controller.submitEditMessage(); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, "Before  after\n\n[asset]: " + urn);
    }

    void inlineEditSaveUsesCurrentParsedAttachments_data()
    {
        QTest::addColumn<QString>("original"); QTest::addColumn<QString>("edited"); QTest::addColumn<QString>("expected");
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt>)";
        const QString image = "![photo](<urn:image:" + messageId + "?name=photo.png>)";
        const QString changed = "![other](<urn:image:" + secondTopicId + "?name=other.png>)";
        const QString code = "Example:\n\n```md\n" + file + "\n```";
        QTest::newRow("retained-trailing-file-and-code") << code + "\n\n" + file << code << file + "\n" + code;
        QTest::newRow("retained-trailing-image-and-code-span") << "`" + image + "`\n\n" + image
            << "`" + image + "`" << image + "\n`" + image + "`";
        QTest::newRow("retained-trailing-file-and-escape") << "\\" + file + "\n\n" + file
            << "\\" + file << file + "\n\\" + file;
        QTest::newRow("deleted-inline") << file + "\nBefore " + image + " after" << QString("Before  after")
            << file + "\nBefore  after";
        QTest::newRow("changed-inline") << file + "\nBefore " + image + " after" << "Before " + changed + " after"
            << file + "\nBefore " + changed + " after";
        QTest::newRow("inline-changed-to-code") << "Before " + image + " after" << "Before `" + image + "` after"
            << "Before `" + image + "` after";
        QTest::newRow("reference-use-deleted") << "Before ![photo][asset] after\n\n[asset]: urn:image:" + messageId
            << "Before  after\n\n[asset]: urn:image:" + messageId << "Before  after\n\n[asset]: urn:image:" + messageId;
        QTest::newRow("reference-definition-deleted") << "Before ![photo][asset] after\n\n[asset]: urn:image:" + messageId
            << QString("Before ![photo][asset] after") << QString("Before ![photo][asset] after");
    }
    void inlineEditSaveUsesCurrentParsedAttachments()
    {
        QFETCH(QString, original); QFETCH(QString, edited); QFETCH(QString, expected);
        Fixture fixture; QString saved; auto row = message(original, returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft(edited); fixture.controller.submitEditMessage();
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, expected); // real outgoing PUT, not a parser-only assertion
    }

    void inlineEditReferenceCardTracksItsCurrentDefinition()
    {
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt>)";
        const QString prefix = "Before ![photo][asset] after\n\n[asset]: ";
        const QString oldReference = "urn:image:" + messageId + "?name=old.png";
        const QString newReference = "urn:image:" + secondTopicId + "?name=new.png";
        Fixture fixture; QString saved; auto row = message(file + "\n" + prefix + oldReference, returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start()); QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto before = fixture.controller.editAttachments(); QCOMPARE(before.size(), 2);
        fixture.controller.setDraft(prefix + newReference);
        const auto after = fixture.controller.editAttachments(); QCOMPARE(after.size(), 2);
        QCOMPARE(after[0].toMap()["id"], before[0].toMap()["id"]); // unrelated retained card stays stable
        QCOMPARE(after[1].toMap()["reference"].toString(), newReference);
        QVERIFY(after[1].toMap()["id"] != before[1].toMap()["id"]);
        fixture.controller.submitEditMessage(); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, file + "\n" + prefix + newReference);
    }

    void inlineEditTypingInReplyTabsKeepsTheirOwnAttachmentCards()
    {
        const QString file = "[report.txt](<urn:file:" + returnedId + "?name=report.txt>)";
        const QString image = "![photo](<urn:image:" + messageId + "?name=photo.png>)";
        const QString secondId = "7b7b7b7b-7b7b-4b7b-8b7b-7b7b7b7b7b7b";
        const QString quote1 = "[One](urn:quote:" + messageId + ")", quote2 = "[Two](urn:quote:" + secondId + ")";
        Fixture fixture; QString saved;
        auto row = message(quote1 + "\n\n" + file + "\nFirst " + image + " answer\n\n" + quote2 + "\n\n" + file + "\nSecond " + image + " answer", returnedId);
        const auto one = message("Origin one", messageId), two = message("Origin two", secondId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT") { saved = request.body["payload"].toObject()["content"].toString(); row["payload"] = request.body["payload"]; }
            if (request.target.path().endsWith("/messages/")) return {QJsonArray{one, two, row}};
            return {request.target.path().endsWith(messageId) ? one : request.target.path().endsWith(secondId) ? two : row};
        };
        QVERIFY(fixture.start()); fixture.controller.setDraft("Stored draft");
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto firstCards = fixture.controller.editAttachments(); QCOMPARE(firstCards.size(), 2);
        const auto tabs = fixture.controller.replyTabs(); QCOMPARE(tabs.size(), 2);
        QVERIFY(fixture.controller.selectReplyTab(tabs[1].toMap()["id"].toString()));
        const auto secondCards = fixture.controller.editAttachments(); QCOMPARE(secondCards.size(), 2);
        fixture.controller.setDraft("Second edited answer");
        QCOMPARE(fixture.controller.editAttachments().size(), 1);
        QCOMPARE(fixture.controller.editAttachments()[0].toMap()["id"], secondCards[0].toMap()["id"]);
        QVERIFY(fixture.controller.selectReplyTab(tabs[0].toMap()["id"].toString()));
        QCOMPARE(fixture.controller.editAttachments(), firstCards);
        fixture.controller.submitEditMessage(); QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(saved, quote1 + "\n\n" + file + "\nFirst " + image + " answer\n\n" + quote2 + "\n\n" + file + "\nSecond edited answer");
        QCOMPARE(fixture.controller.draft(), QString("Stored draft"));
        QVERIFY(fixture.controller.beginEditMessage(returnedId)); fixture.controller.setDraft("Cancelled body");
        fixture.controller.cancelEditMessage(); QVERIFY(fixture.controller.editAttachments().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("Stored draft"));
    }

    void inlineEditSessionKeepsTheStoredDraftAndClosesOnAConfirmedSave()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        int edits = 0, sends = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") { ++edits; newer["payload"] = request.body.value("payload"); return {newer}; }
            if (request.method == "POST") { ++sends; return {message("racing send", returnedId), 201}; }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("half-written note");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        // A Source edit session loads the message body into the composer and never touches
        // the stored draft (message-composer-draft.hook.ts never calls onValueChange while editing).
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message"));
        QVERIFY(fixture.controller.canSaveEdit());
        QVERIFY(!fixture.controller.canSend());
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        // Typing inside the session rewrites only the edit buffer.
        fixture.controller.setDraft("Second message, edited");
        QCOMPARE(fixture.controller.draft(), QString("Second message, edited"));
        QTest::qWait(200);
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        // A blank buffer cannot be saved (reference: send disabled when value.trim() is empty).
        fixture.controller.setDraft("   ");
        QVERIFY(!fixture.controller.canSaveEdit());
        fixture.controller.submitEditMessage();
        QTest::qWait(200);
        QCOMPARE(edits, 0);
        QVERIFY(fixture.controller.editingMessageId() == returnedId);
        fixture.controller.setDraft("Second message, edited");

        // Sending a new message while a session is open is refused.
        fixture.controller.send();
        QTest::qWait(200);
        QCOMPARE(sends, 0);

        // A confirmed server result closes the session and the stored draft returns.
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 1);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, edited"));
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("half-written note"));
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));
        QVERIFY(!fixture.controller.canSaveEdit());
        QVERIFY(fixture.controller.canSend());
    }

    void inlineEditCancelFailureAndRouteChangePreserveTheStoredDraft()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); fixture.extraTopic = true;
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        int edits = 0; bool failing = false;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId
                    ? QJsonArray{older, newer} : QJsonArray{}};
            if (request.method == "PUT") {
                if (failing) return {QJsonObject{{"detail", "nope"}}, 500};
                ++edits; newer["payload"] = request.body.value("payload"); return {newer};
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("work in progress");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("work in progress"));

        // Cancelling restores the pre-edit draft and stores nothing.
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, half typed");
        fixture.controller.cancelEditMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("work in progress"));
        QCOMPARE(drafts.text(streamId, topicId), QString("work in progress"));
        QCOMPARE(edits, 0);

        // A failed save keeps the session and the typed text open with an error.
        failing = true;
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, retry soon");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message, retry soon"));
        fixture.controller.cancelEditMessage();
        QCOMPARE(fixture.controller.draft(), QString("work in progress"));

        // Leaving the conversation ends the session (reference clears the edit session on route change).
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.selectTopic(secondTopicId);
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QVERIFY(!fixture.controller.canSaveEdit());

        // Up-to-edit-last picks the newest own markdown message.
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.editLastMessage();
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        fixture.controller.cancelEditMessage();

        // A message that cannot be edited reports instead of starting a session.
        fixture.controller.beginEditMessage("00000000-0000-4000-8000-000000000000");
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QVERIFY(!fixture.controller.error().isEmpty());
        const QString refusal = fixture.controller.error();
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.editLastMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.error(), refusal);
    }

    // Issue #115: an in-flight edit save belongs to the exact session and buffer revision it
    // submitted. Text typed after Save must survive the completion of the earlier save, and
    // while one save is in flight a second PUT for the same message is refused (the save
    // control is disabled), so concurrent PUTs for one message can never conflict.
    void inlineEditSaveCompletionCannotDiscardTextTypedAfterSave()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        int edits = 0; bool holdFirstPut = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                ++edits;
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                if (holdFirstPut) {
                    holdFirstPut = false;
                    response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                }
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("half-written note");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));

        // Text entered after Save must stay in the buffer while the save is pending, and
        // a second save of the same message cannot be submitted before the first lands.
        fixture.controller.setDraft("Second message, typed B");
        QCOMPARE(fixture.controller.draft(), QString("Second message, typed B"));
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QVERIFY(!fixture.controller.canSaveEdit());

        // Completing the A save verifies body A but must not close the session or clear B:
        // the completion belongs to an older buffer revision of the session.
        releaseSave(); releaseSave = {};
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, saved A"));
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message, typed B"));
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));
        QTRY_VERIFY(fixture.controller.canSaveEdit());

        // B is savable now; its own confirmation closes the session and the ordinary draft returns.
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 2);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, typed B"));
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("half-written note"));
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));
    }

    // Issue #115 trigger 3b: cancel and reopen the same message while the earlier save is
    // still in flight. The completion belongs to the closed session and must not touch the
    // reopened one.
    void inlineEditSaveCompletionCannotCloseAReopenedSession()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        int edits = 0; bool holdFirstPut = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                ++edits;
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                if (holdFirstPut) {
                    holdFirstPut = false;
                    response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                }
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("ordinary draft");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("ordinary draft"));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));

        fixture.controller.cancelEditMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("ordinary draft"));

        // Reopen the same UUID while the earlier PUT is still in flight.
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, typed B");

        releaseSave(); releaseSave = {};
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, saved A"));
        // The reopened session survives the old completion untouched.
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message, typed B"));
        QTRY_VERIFY(fixture.controller.canSaveEdit());

        fixture.controller.cancelEditMessage();
        QCOMPARE(fixture.controller.draft(), QString("ordinary draft"));
        QCOMPARE(drafts.text(streamId, topicId), QString("ordinary draft"));
        QCOMPARE(edits, 1);
    }

    // Issue #115: repeated saves of one session cannot run concurrently; a second submit
    // while the first PUT is pending is refused outright.
    void inlineEditSecondSaveIsRefusedWhileFirstIsPending()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        int edits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                ++edits;
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));
        QCOMPARE(edits, 1);

        // Same buffer, second submit: refused, no second PUT goes out.
        QVERIFY(!fixture.controller.canSaveEdit());
        fixture.controller.submitEditMessage();
        QTest::qWait(250);
        QCOMPARE(edits, 1);

        releaseSave(); releaseSave = {};
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, saved A"));
        // The buffer was never changed, so this completion does close the session.
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(edits, 1);
    }

    // The in-flight guard feeds the canSaveEdit property, so every transition of the guard
    // must notify editSessionChanged (the property's NOTIFY signal), otherwise a QML save
    // control bound to canSaveEdit keeps a stale enabled state after the save settles.
    void inlineEditSaveInFlightTransitionsNotifyTheSaveControl()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        bool holdFirstPut = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                if (holdFirstPut) {
                    holdFirstPut = false;
                    response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                }
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        QSignalSpy notified(&fixture.controller, &NativeController::editSessionChanged);
        QVERIFY(notified.isValid());

        // Taking the save in flight flips canSaveEdit to false, so it must notify.
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));
        QVERIFY(!fixture.controller.canSaveEdit());
        QVERIFY2(notified.count() >= 1, "in-flight insertion must emit editSessionChanged");

        // Type after Save so the completion lands on a newer revision and the session
        // stays open; clearing the guard flips canSaveEdit back and must notify again.
        fixture.controller.setDraft("Second message, typed B");
        notified.clear();
        releaseSave(); releaseSave = {};
        QTRY_VERIFY(fixture.controller.canSaveEdit());
        QVERIFY2(notified.count() >= 1, "in-flight completion must emit editSessionChanged");
    }

    // Reference contract (workspace_ui 61dacef): features/workspace-reply
    // serializes a reply as "[Name](urn:quote:<uuid>)" + blank line + answer
    // (workspace-reply.lib.ts:190-212); the composer preview header is
    // "Reply: Name" with an 80-character summary body
    // (message-composer-reply-quote-preview.ui.tsx, QUOTE_PREVIEW_MAX=80 in
    // message-composer-constants.lib.ts:3).

    void replySessionComposesTheQuoteLineOnAConfirmedSend()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        auto note = message("Peer note\nsecond line", messageId);
        note["is_own"] = false; note["author_uuid"] = peerId;
        QJsonObject posted; int posts = 0;
        const auto savedContent = [&]{ return posted.value("payload").toObject().value("content").toString(); };
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST") {
                ++posts; posted = request.body;
                auto saved = message(savedContent(), returnedId);
                return {saved, 201};
            }
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{note}};
            return {message(savedContent(), returnedId)};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("carry-over answer");

        QVERIFY(fixture.controller.beginReply(messageId));
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
        QTRY_COMPARE(fixture.controller.replyAuthor(), QString("Bobby"));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Peer note second line"));
        // The answer must not be blank: canSend stays gated on the draft
        // (reference disables send when the composer value is empty).
        fixture.controller.setDraft("");
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.send();
        QTest::qWait(200);
        QCOMPARE(posts, 0);

        // Starting a reply keeps whatever the composer already holds as the answer.
        fixture.controller.setDraft("my answer");
        QVERIFY(fixture.controller.canSend());
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QCOMPARE(savedContent(), QString("[Bobby](urn:quote:%1)\n\nmy answer").arg(messageId));
        QTRY_VERIFY(fixture.controller.replyMessageId().isEmpty());
        QTRY_VERIFY(fixture.controller.draft().isEmpty());

        // The echoed message comes back with the quote frame in its row map.
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("quoteId").toString(), messageId);
        QCOMPARE(row.value("displayContent").toString(), QString("my answer"));
        // A leading anchor carries its author in the link text: the frame renders
        // ready without any extra fetch (docs/NATIVE_DESKTOP_REPLIES.md).
        QCOMPARE(row.value("quoteAuthorName").toString(), QString("Bobby"));
        QCOMPARE(row.value("quoteStatus").toString(), QString("ready"));
    }

    // Issue #217 (docs/NATIVE_DESKTOP_QUOTE_MEDIA_THUMBNAILS.md): the quoted body
    // picks one preview reference exactly like the source
    // (messenger-workspace-message-body-files.lib.ts:22-35 at workspace_ui
    // 230e02b8) - the first image wins, otherwise the first video.
    void quotePreviewPicksFirstImageThenFirstVideo()
    {
        Fixture fixture; QVERIFY(fixture.start());
        const QString image = "urn:image:6b6b6b6b-6b6b-4b6b-8b6b-6b6b6b6b6b61"
            "?name=first.png&content_type=image%2Fpng&w=20&h=20&size=20";
        const QString other = "urn:image:6b6b6b6b-6b6b-4b6b-8b6b-6b6b6b6b6b62"
            "?name=second.png&content_type=image%2Fpng&w=20&h=20&size=20";
        const QString clip = "urn:video:6b6b6b6b-6b6b-4b6b-8b6b-6b6b6b6b6b63"
            "?name=clip.mp4&content_type=video%2Fmp4&w=32&h=18&size=30";

        const auto single = fixture.controller.mediaPreviewReference("![first.png](" + image + ")");
        QCOMPARE(single.value("urn").toString(), image);
        QCOMPARE(single.value("kind").toString(), QString("image"));

        // Document order keeps the first image when several attachments follow.
        const auto several = fixture.controller.mediaPreviewReference(
            "Body\n\n![first.png](" + image + ")\n\n![second.png](" + other + ")");
        QCOMPARE(several.value("urn").toString(), image);

        // A video that comes first never wins over a later image.
        const auto mixed = fixture.controller.mediaPreviewReference(
            "Body\n\n![clip.mp4](" + clip + ")\n\n![first.png](" + image + ")");
        QCOMPARE(mixed.value("urn").toString(), image);
        QCOMPARE(mixed.value("kind").toString(), QString("image"));

        // Without an image the first video is the preview.
        const auto video = fixture.controller.mediaPreviewReference("Body\n\n![clip.mp4](" + clip + ")");
        QCOMPARE(video.value("urn").toString(), clip);
        QCOMPARE(video.value("kind").toString(), QString("video"));

        QVERIFY(fixture.controller.mediaPreviewReference("Plain body only").isEmpty());
        QVERIFY(fixture.controller.mediaPreviewReference(QString()).isEmpty());
    }

    // The composer reply preface carries the picked reference and the summary the
    // source shows once the thumbnail is ready: every media markdown token is
    // dropped from the text (message-composer-reply-quote-preview.ui.tsx:74-83).
    void replyPreviewSummarizesWithoutTheMediaLabelOnceReady()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString image = "urn:image:6b6b6b6b-6b6b-4b6b-8b6b-6b6b6b6b6b64"
            "?name=shot.png&content_type=image%2Fpng&w=20&h=20&size=20";
        auto note = message("Peer caption\n\n![shot.png](" + image + ")", messageId);
        note["is_own"] = false; note["author_uuid"] = peerId;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET") return {QJsonArray{note}};
            return {message("answer", returnedId), 201};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginReply(messageId));
        QCOMPARE(fixture.controller.replyMediaUrn(), image);
        QCOMPARE(fixture.controller.replyMediaKind(), QString("image"));
        // Before the thumbnail arrives the media label stays in the summary.
        QVERIFY(fixture.controller.replyExcerpt().contains("shot.png"));
        // Once it is ready the media token is hidden from the summary.
        QCOMPARE(fixture.controller.replyMediaSummary(), QString("Peer caption"));
    }

    void replyPreviewWithoutMediaHasNoThumbnail()
    {
        Fixture fixture;
        auto note = message("Plain peer note", messageId);
        note["is_own"] = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET") return {QJsonArray{note}};
            return {message("answer", returnedId), 201};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.beginReply(messageId));
        QVERIFY(fixture.controller.replyMediaUrn().isEmpty());
        QVERIFY(fixture.controller.replyMediaKind().isEmpty());
        // With no media token to hide the summary is the ordinary excerpt.
        QCOMPARE(fixture.controller.replyMediaSummary(), fixture.controller.replyExcerpt());
    }

    // Reply to selected text (issue #218), reference workspace_ui 230e02b8:
    // the bubble menu hands getSelectedText to beginReply/addReplyTab
    // (workspace-message-bubble-menu.ui.tsx:295-317); a whitespace-only or
    // encrypted-fragment selection drops to the whole-message quote
    // (chat-page-workspace.ui.tsx:2747-2751, bubble-presentation.ui.tsx:636-639);
    // the selection wins the 36-char tab excerpt (workspace-reply.ui.tsx:28-32);
    // the wire is urn:quote:<uuid>?text=<percent-encoded>.
    void replySelectedTextWinsQuoteLabelAndWire()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        auto note = message("Peer note\nsecond line", messageId);
        note["is_own"] = false; note["author_uuid"] = peerId;
        const QString secretId = "7e7e7e7e-7e7e-4e7e-8e7e-7e7e7e7e7e7e";
        const QString restoreId = "0b0b0b0b-0b0b-4b0b-8b0b-0b0b0b0b0b0b";
        auto secretNote = message(
            QStringLiteral("Peer note [redacted](urn:secret:11111111-1111-4111-8111-111111111111)"), secretId);
        secretNote["is_own"] = false; secretNote["author_uuid"] = peerId;
        const auto restored = message(QStringLiteral("[Bobby](urn:quote:%1?text=second%20line)\n\nkeep answer")
            .arg(messageId), restoreId);
        QJsonObject posted; int posts = 0;
        const auto savedContent = [&]{ return posted.value("payload").toObject().value("content").toString(); };
        fixture.aggregateMessages = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto query = QUrlQuery(request.target).query(QUrl::FullyEncoded);
            if (request.method == "POST") { ++posts; posted = request.body; return {message(savedContent(), returnedId), 201}; }
            if (request.method == "GET" && (query.contains("uuid=") || path.endsWith("/messages/"))) return {QJsonArray{note, secretNote, restored}};
            return {message(savedContent(), returnedId)};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("answer");

        // The selection wins the banner excerpt (whitespace collapsed) and the
        // tab label (workspace-reply.ui.tsx:28-37).
        QVERIFY(fixture.controller.beginReply(messageId, QStringLiteral(" second  line ")));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("second line"));
        auto rows = fixture.controller.replyTabs().toList();
        QCOMPARE(rows.first().toMap().value("label").toString(), QString("Bobby: second line"));

        // A whitespace-only selection is dropped: whole-message quote returns
        // and the tab keeps no selection.
        QVERIFY(fixture.controller.beginReply(messageId, QStringLiteral("   ")));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Peer note second line"));

        // An empty selection never emits "?text=" on the wire.
        QVERIFY(fixture.controller.beginReply(messageId, QString()));
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(savedContent(), QString("[Bobby](urn:quote:%1)\n\nanswer").arg(messageId));

        // A selection serializes percent-encoded (Cyrillic and reserved chars).
        QVERIFY(fixture.controller.beginReply(messageId, QStringLiteral("мир & <x>")));
        fixture.controller.setDraft("ok");
        QTRY_VERIFY(fixture.controller.canSend());
        fixture.controller.send();
        QTRY_COMPARE(posts, 2);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY2(savedContent().contains(QStringLiteral("urn:quote:") + messageId
            + QStringLiteral("?text=%D0%BC%D0%B8%D1%80%20%26%20%3Cx%3E")), qPrintable(savedContent()));

        // The 36-char collapse applies to the fragment: slice(0, 36) trimEnd + "…".
        QVERIFY(fixture.controller.beginReply(messageId,
            QStringLiteral("selection0selection1selection2selection3selection4")));
        rows = fixture.controller.replyTabs().toList();
        QCOMPARE(rows.first().toMap().value("excerpt").toString(),
            QStringLiteral("selection0selection1selection2select") + QString::fromUtf8("\xE2\x80\xA6"));

        // A bubble containing encrypted fragments never quotes the selection
        // (revealed text stays out of the wire); the reply falls back to the
        // whole message quote instead (bubble-presentation.ui.tsx:636-639).
        QVERIFY(fixture.controller.beginReply(secretId, QStringLiteral("chosen bit")));
        const auto secretExcerpt = fixture.controller.replyExcerpt();
        QVERIFY2(!secretExcerpt.contains(QLatin1String("chosen")), qPrintable(secretExcerpt));
        QVERIFY2(secretExcerpt.startsWith(QLatin1String("Peer note")), qPrintable(secretExcerpt));

        // addReplyTab carries the selection onto the appended tab too (the
        // reference passes getSelectedText to the add-reply sibling).
        QVERIFY(fixture.controller.addReplyTab(messageId, QStringLiteral("second line")));
        rows = fixture.controller.replyTabs().toList();
        QCOMPARE(rows.last().toMap().value("excerpt").toString(), QString("second line"));
        for (const auto &value : rows)
            fixture.controller.removeReplyTab(value.toMap().value("id").toString());
        QVERIFY(fixture.controller.replyMessageId().isEmpty());

        // Edit restore reads "?text=" back: the restored tab quotes the
        // fragment, and the verbatim reference re-serializes unchanged.
        QVERIFY(fixture.controller.beginEditMessage(restoreId));
        QTRY_COMPARE(fixture.controller.replyExcerpt(), QString("second line"));
        // The verbatim tab.reference (already shipping, docs/
        // NATIVE_DESKTOP_EDIT_REPLY_RESTORE.md) keeps the "?text=" identity on
        // the next save; this lane only added the visible-fragment half.
        fixture.controller.cancelEditMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
    }

    // Issue #218 review r2, finding 2: the body TextEdit hands the menu
    // snapshot its selectedText, where TextEdit encodes paragraph breaks as
    // U+2029 (and hard line breaks as U+2028). The serialized quote fragment
    // must carry the selected text's newlines: urn:quote:...?text=<...%0A...>,
    // never the raw paragraph separators.
    void replySelectionParagraphSeparatorsSerializeAsNewlines()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        auto note = message("First line\nsecond line", messageId);
        note["is_own"] = false; note["author_uuid"] = peerId;
        QJsonObject posted; int posts = 0;
        const auto savedContent = [&]{ return posted.value("payload").toObject().value("content").toString(); };
        fixture.aggregateMessages = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto query = QUrlQuery(request.target).query(QUrl::FullyEncoded);
            if (request.method == "POST") { ++posts; posted = request.body; return {message(savedContent(), returnedId), 201}; }
            if (request.method == "GET" && (query.contains("uuid=") || path.endsWith("/messages/"))) return {QJsonArray{note}};
            return {message(savedContent(), returnedId)};
        };
        QVERIFY(fixture.start());

        // A U+2029 paragraph break (multi-paragraph drag) serializes as %0A.
        QVERIFY(fixture.controller.beginReply(messageId, QStringLiteral("line\u2029second")));
        const auto rows = fixture.controller.replyTabs().toList();
        QVERIFY2(!rows.first().toMap().value("label").toString().contains(QChar(0x2029)),
            qPrintable(rows.first().toMap().value("label").toString()));
        fixture.controller.setDraft("note");
        QTRY_VERIFY(fixture.controller.canSend());
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY2(savedContent().contains(QStringLiteral("urn:quote:") + messageId
            + QStringLiteral("?text=line%0Asecond")), qPrintable(savedContent()));
        QVERIFY2(!savedContent().contains(QLatin1String("E2%80%A9"))
            && !savedContent().contains(QLatin1String("e2%80a9")), qPrintable(savedContent()));

        // A U+2028 hard line break normalizes the same way.
        QVERIFY(fixture.controller.beginReply(messageId, QStringLiteral("line\u2028second")));
        fixture.controller.setDraft("note");
        QTRY_VERIFY(fixture.controller.canSend());
        fixture.controller.send();
        QTRY_COMPARE(posts, 2);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY2(savedContent().contains(QStringLiteral("urn:quote:") + messageId
            + QStringLiteral("?text=line%0Asecond")), qPrintable(savedContent()));
        QVERIFY2(!savedContent().contains(QLatin1String("E2%80%A8"))
            && !savedContent().contains(QLatin1String("e2%80a8")), qPrintable(savedContent()));
    }

    // An inline (non-leading) urn:quote anchor stays in the body text for the
    // markdown renderer, and openMessageInChat resolves the origin for the click.
    void replyAnchorInsideTheBodyStaysRenderableAndNavigable()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        auto note = message("Peer note", messageId);
        note["is_own"] = false; note["author_uuid"] = peerId;
        QJsonObject posted; int posts = 0; int lookups = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST") {
                ++posts; posted = request.body;
                return {message(posted.value("payload").toObject().value("content").toString(), returnedId), 201};
            }
            if (request.method == "GET" && path.endsWith("/messages/")) {
                ++lookups;
                return {QJsonArray{note}};
            }
            return {message(posted.value("payload").toObject().value("content").toString(), returnedId)};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("note [Bobby](urn:quote:" + messageId + ") above");
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QVariantMap row;
        // The echo lands asynchronously; wait for the row instead of racing it.
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return !row.isEmpty(); })(), 5000);
        QCOMPARE(row.value("quoteId").toString(), QString());
        QVERIFY(row.value("displayContent").toString().contains(QLatin1String("(urn:quote:")));
        // Clicking the anchor routes to the origin: the shipped message-link
        // machinery resolves it and arms the highlight flash on the row.
        fixture.controller.openMessageInChat(messageId);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), messageId);
    }

    // A transport failure while resolving a quote must not freeze the preview:
    // the reference loader keeps retriable failures out of the resolved store
    // (messenger-quote-loader.lib.ts distinguishes stale from unavailable).
    void transientQuoteLookupFailureRecoversAfterTheNetworkReturns()
    {
        Fixture fixture;
        fixture.aggregateMessages = true; // route every history GET to this handler (quote lookups included)
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        const auto far = message("far origin", farId);
        int lookups = 0; bool recover = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonArray{reply}};
            if (QUrlQuery(request.target).hasQueryItem("uuid")) {
                ++lookups;
                if (!recover) return {QJsonObject{}, 500};
                return {QJsonArray{far}};
            }
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            row = modelMessage(fixture.controller, returnedId);
            return lookups >= 1 && !row.isEmpty();
        })(), 5000);
        QTest::qWait(300);
        row = modelMessage(fixture.controller, returnedId);
        QVERIFY2(row.value("quoteStatus").toString() != QLatin1String("unavailable"),
                 qPrintable(row.value("quoteStatus").toString()));
        recover = true;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready");
        })(), 9000);
        QCOMPARE(row.value("quoteContent").toString(), QString("far origin"));
    }

    // A confirmed edit of the origin must flow into every quote preview that
    // references it, even when the origin lives in another conversation
    // (reference keeps quote content inside the shared message store).
    void quotePreviewFollowsAConfirmedEditOfTheOrigin()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        fixture.aggregateMessages = true; // route quote/verification GETs to this handler
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        QString originContent = "far origin"; int puts = 0;
        const auto fresh = [&] { auto m = message(originContent, farId, secondTopicId); return m; };
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto query = QUrlQuery(request.target);
            if (request.method == "PUT") {
                ++puts; originContent = request.body.value("payload").toObject().value("content").toString();
                return {QJsonObject{}, 200};
            }
            if (path.endsWith("/" + farId)) return {fresh()};
            if (query.hasQueryItem("uuid")) return {QJsonArray{fresh()}};
            if (query.queryItemValue("topic_uuid") == secondTopicId) return {QJsonArray{fresh()}};
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready"); })(), 5000);
        QCOMPARE(row.value("quoteContent").toString(), QString("far origin"));

        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!modelMessage(fixture.controller, farId).isEmpty());
        QVERIFY(fixture.controller.beginEditMessage(farId));
        fixture.controller.setDraft("edited origin");
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(puts, 1);
        // The session closes only after the server confirms the mutation; wait for
        // that before leaving the conversation so the verification is not cancelled.
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());

        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteContent").toString() == QLatin1String("edited origin"); })(), 5000);
    }

    void quoteLookupWorkRetiresWithItsSession_data()
    {
        QTest::addColumn<QString>("transition");
        QTest::newRow("logout-before-retry") << QString("logout");
        QTest::newRow("switch-before-retry") << QString("switch");
        QTest::newRow("reset-before-queued-dispatch") << QString("queued");
    }
    void quoteLookupWorkRetiresWithItsSession()
    {
        QFETCH(QString, transition);
        Fixture fixture, successor;
        fixture.aggregateMessages = true;
        successor.aggregateMessages = true;
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QString("[Bobby](urn:quote:%1)\n\nanswer").arg(farId), returnedId);
        int firstLookups = 0, successorLookups = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).hasQueryItem("uuid")) {
                ++firstLookups;
                return {QJsonObject{}, 500};
            }
            return {QJsonArray{reply}};
        };
        successor.messages = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).queryItemValue("uuid") == farId) ++successorLookups;
            return {QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("quoteStatus").toString(), QString("loading"));
        if (transition == "queued") {
            // Projection only queued dispatch: no event-loop turn has sent it.
            QCOMPARE(firstLookups, 0);
        } else {
            QTRY_COMPARE(firstLookups, 1);
            QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        }
        fixture.controller.logout();
        QVERIFY(!fixture.controller.authenticated());
        if (transition == "switch") {
            QVERIFY(fixture.start(successor.server.origin()));
            QVERIFY(fixture.controller.authenticated());
            QVERIFY(fixture.controller.messages().isEmpty());
        }
        // No active conversation needs the old UUID. This watches local failures
        // too: while signed out the old timer cannot reach HTTP but used to emit
        // another quote update and arm yet another retry.
        QSignalSpy updates(&fixture.controller, &NativeController::messagesChanged);
        QTest::qWait(3700);
        QCOMPARE(successorLookups, 0);
        QCOMPARE(firstLookups, transition == "queued" ? 0 : 1);
        QCOMPARE(updates.size(), 0);
        QVERIFY(fixture.controller.messages().isEmpty());
    }

    void quoteLookupNewGenerationCanResolveAgain_data()
    {
        QTest::addColumn<bool>("inFlight");
        QTest::newRow("retired-in-flight") << true;
        QTest::newRow("retired-backoff") << false;
    }
    void quoteLookupNewGenerationCanResolveAgain()
    {
        QFETCH(bool, inFlight);
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QString("[Bobby](urn:quote:%1)\n\nanswer").arg(farId), returnedId);
        int lookups = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (!QUrlQuery(request.target).hasQueryItem("uuid")) return {QJsonArray{reply}};
            ++lookups;
            if (lookups == 1) return {QJsonObject{}, 500, inFlight ? 600 : 0};
            return {QJsonArray{message("recovered quote", farId)}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("quoteStatus").toString(), QString("loading"));
        QTRY_COMPARE(lookups, 1);
        if (!inFlight) QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        fixture.api.setAccessToken("synthetic-quote-renewal");
        // A new view demand must not inherit a cancelled request's in-flight
        // entry or its backoff; the old generation's timer is retired as well.
        QTRY_COMPARE_WITH_TIMEOUT(modelMessage(fixture.controller, returnedId).value("quoteContent").toString(),
            QString("recovered quote"), 1200);
        QCOMPARE(lookups, 2);
        QTest::qWait(3700);
        QCOMPARE(lookups, 2);
    }

    // After a transport failure the retry is self-driving: the lookup re-issues
    // even though the test performs no further projections (review #119, round 2).
    void failedQuoteLookupRetriesWithoutFurtherProjections()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        const auto far = message("far origin", farId);
        int lookups = 0; bool recover = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonArray{reply}};
            if (QUrlQuery(request.target).hasQueryItem("uuid")) {
                ++lookups;
                if (!recover) return {QJsonObject{}, 500};
                return {QJsonArray{far}};
            }
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return lookups >= 1 && !row.isEmpty(); })(), 5000);
        // No further projection happens here; only a self-scheduled timer can make
        // the lookup count grow.
        recover = true;
        const int baseline = lookups;
        QTest::qWait(6000);
        QVERIFY2(lookups > baseline, "the failed lookup never retried by itself");
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready"); })(), 9000);
    }

    // A confirmed deletion of the origin drops the cached preview; the frame then
    // re-resolves against the server and lands on unavailable (review #119).
    void quotePreviewBecomesUnavailableAfterTheOriginIsDeleted()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        fixture.aggregateMessages = true;
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        bool deleted = false; int deletes = 0;
        const auto fresh = [&] { return message("far origin", farId, secondTopicId); };
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto query = QUrlQuery(request.target);
            if (request.method == "DELETE") { deleted = true; ++deletes; return {QJsonObject{}, 200}; }
            if (request.method != "GET") return {QJsonArray{reply}};
            if (path.endsWith("/" + farId)) return deleted ? Response{QJsonObject{}, 404} : Response{fresh()};
            if (query.hasQueryItem("uuid")) return deleted ? Response{QJsonArray{}} : Response{QJsonArray{fresh()}};
            if (query.queryItemValue("topic_uuid") == secondTopicId)
                return deleted ? Response{QJsonArray{}} : Response{QJsonArray{fresh()}};
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready"); })(), 5000);

        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!modelMessage(fixture.controller, farId).isEmpty());
        fixture.controller.deleteMessage(farId);
        QTRY_COMPARE(deletes, 1);

        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("unavailable"); })(), 5000);
    }

    // A message consisting of only a quote anchor keeps an empty answer body;
    // the frame replaces the anchor instead of the body duplicating it.
    void quoteOnlyMessageKeepsAnEmptyAnswerBody()
    {
        Fixture fixture;
        auto quoteOnly = message(QStringLiteral("[Aela](urn:quote:%1)").arg(messageId), returnedId);
        quoteOnly["is_own"] = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonArray{}};
            return {QJsonArray{message("Peer note", messageId), quoteOnly}};
        };
        QVERIFY(fixture.start());
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("quoteId").toString(), messageId);
        QVERIFY(row.contains("displayContent"));
        QCOMPARE(row.value("displayContent").toString(), QString());
    }

    void replyCancelFailureAndRouteChangePreserveTheAnswer()
    {
        Fixture fixture; fixture.extraTopic = true;
        auto note = message("Peer note", messageId);
        note["is_own"] = false;
        auto ownNote = message("Own note", returnedId);
        ownNote["created_at"] = "2026-01-02T10:00:01.000Z";
        bool failing = false; int posts = 0;
        QString postedContent;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST") {
                if (failing) return {QJsonObject{{"detail", "nope"}}, 500};
                ++posts;
                postedContent = request.body.value("payload").toObject().value("content").toString();
                return {message(postedContent, returnedId), 201};
            }
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId
                    ? QJsonArray{note, ownNote} : QJsonArray{}};
            return {message(postedContent, returnedId)};
        };
        QVERIFY(fixture.start());

        // Cancel closes the session and leaves the typed answer alone.
        QVERIFY(fixture.controller.beginReply(messageId));
        fixture.controller.setDraft("half typed");
        fixture.controller.cancelReply();
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("half typed"));

        // A failed send keeps both the session and the answer (edit-session precedent).
        failing = true;
        QVERIFY(fixture.controller.beginReply(messageId));
        fixture.controller.send();
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
        QCOMPARE(fixture.controller.draft(), QString("half typed"));
        QCOMPARE(posts, 0);
        failing = false;
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QTRY_VERIFY(fixture.controller.replyMessageId().isEmpty());

        // Leaving a conversation parks the session under its own conversation
        // key (issue #201): B shows its own (empty) session while A's stays
        // parked with the typed answer.
        QVERIFY(fixture.controller.beginReply(messageId));
        fixture.controller.setDraft("parked answer");
        fixture.controller.selectTopic(secondTopicId);
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QTRY_VERIFY(!fixture.controller.busy());

        // Returning restores A's session unchanged; a message that cannot be
        // quoted reports instead of touching the restored session.
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QTRY_COMPARE(fixture.controller.replyMessageId(), messageId);
        QCOMPARE(fixture.controller.draft(), QString("parked answer"));
        QVERIFY(!fixture.controller.beginReply("00000000-0000-4000-8000-000000000000"));
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
        QVERIFY(!fixture.controller.error().isEmpty());

        // Reply and edit sessions are mutually exclusive.
        QVERIFY(fixture.controller.beginReply(messageId));
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QVERIFY(fixture.controller.beginReply(messageId));
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
    }

    // Multi-reply tabs (docs/NATIVE_DESKTOP_MULTI_REPLY_TABS.md): the composer
    // accumulates concurrent quote tabs, each carrying its own answer, and one
    // send serializes every "[Name](urn:quote:<uuid>)" block plus answer in tab
    // order (workspace-reply.lib.ts:190-211 at 61dacef).
    void multiReplyTabsAnswerEachTabAndSendInTabOrder()
    {
        Fixture fixture;
        const QString peerA = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        const QString peerB = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerA}, {"username", "amy"}, {"first_name", "Amy"}},
            QJsonObject{{"uuid", peerB}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString firstId = "11111111-1111-4111-8111-111111111111";
        const QString secondId = "22222222-2222-4222-8222-222222222222";
        auto noteA = message("Alpha note", firstId);
        noteA["is_own"] = false; noteA["author_uuid"] = peerA;
        auto noteB = message("Beta note", secondId);
        noteB["is_own"] = false; noteB["author_uuid"] = peerB;
        QJsonObject posted; int posts = 0;
        const auto savedContent = [&]{ return posted.value("payload").toObject().value("content").toString(); };
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST") { ++posts; posted = request.body; return {message(savedContent(), returnedId), 201}; }
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{noteA, noteB}};
            return {message(savedContent(), returnedId)};
        };
        QVERIFY(fixture.start());

        // The first tab inherits whatever the composer already holds (page wiring
        // 4166-4181: composer text moves into the first tab of an empty session).
        fixture.controller.setDraft("carry over");
        QVERIFY(fixture.controller.beginReply(firstId));
        QCOMPARE(fixture.controller.replyMessageId(), firstId);
        QCOMPARE(fixture.controller.draft(), QString("carry over"));
        fixture.controller.setDraft("answer one");
        // addReplyTab appends and activates a fresh blank-answer tab.
        QVERIFY(fixture.controller.addReplyTab(secondId));
        QCOMPARE(fixture.controller.replyMessageId(), secondId);
        QVERIFY(fixture.controller.draft().isEmpty());
        auto tabs = fixture.controller.replyTabs();
        QCOMPARE(tabs.size(), 2);
        QCOMPARE(tabs[0].toMap().value("message").toString(), firstId);
        QCOMPARE(tabs[1].toMap().value("message").toString(), secondId);
        QTRY_COMPARE(tabs[1].toMap().value("author").toString(), QString("Bobby"));
        QCOMPARE(tabs[1].toMap().value("label").toString(), QString("Bobby: Beta note"));
        fixture.controller.setDraft("answer two");
        // Editing the composer edits only the active tab (workspace-reply.lib.ts:113-127).
        const QString tabA = tabs[0].toMap().value("id").toString();
        QVERIFY(fixture.controller.selectReplyTab(tabA));
        QCOMPARE(fixture.controller.draft(), QString("answer one"));
        fixture.controller.send(); // gated by canSend on the active draft; both tabs go out
        QTRY_COMPARE(posts, 1);
        QCOMPARE(savedContent(), QString("[Amy](urn:quote:%1)\n\nanswer one\n\n[Bobby](urn:quote:%2)\n\nanswer two")
                 .arg(firstId, secondId));
        QTRY_VERIFY(fixture.controller.replyTabs().isEmpty());
        QTRY_VERIFY(fixture.controller.draft().isEmpty());

        // A blank active tab never blocks a send: an answer saved in another tab
        // is sendable on its own. The blank active tab then serializes as the bare
        // quote line with no dangling blank lines (trimEnd contract).
        QVERIFY(fixture.controller.beginReply(firstId));
        fixture.controller.setDraft("only answer");
        QVERIFY(fixture.controller.addReplyTab(secondId));
        QVERIFY(fixture.controller.draft().isEmpty());
        QVERIFY(fixture.controller.canSend());
        fixture.controller.send();
        QTRY_COMPARE(posts, 2);
        QCOMPARE(savedContent(), QString("[Amy](urn:quote:%1)\n\nonly answer\n\n[Bobby](urn:quote:%2)").arg(firstId, secondId));
        QTRY_VERIFY(fixture.controller.replyTabs().isEmpty());

        // Reorder changes the block order in the serialized reply.
        QVERIFY(fixture.controller.beginReply(firstId));
        fixture.controller.setDraft("answer one");
        QVERIFY(fixture.controller.addReplyTab(secondId));
        fixture.controller.setDraft("answer two");
        const QString tabB = fixture.controller.replyTabs()[1].toMap().value("id").toString();
        QVERIFY(fixture.controller.reorderReplyTab(tabB, 0)); // insertion before removal: [B, A]
        tabs = fixture.controller.replyTabs();
        QCOMPARE(tabs[0].toMap().value("message").toString(), secondId);
        QCOMPARE(tabs[1].toMap().value("message").toString(), firstId);
        QCOMPARE(fixture.controller.replyMessageId(), secondId); // active untouched by reorder
        fixture.controller.send();
        QTRY_COMPARE(posts, 3);
        QCOMPARE(savedContent(), QString("[Bobby](urn:quote:%1)\n\nanswer two\n\n[Amy](urn:quote:%2)\n\nanswer one")
                 .arg(secondId, firstId));
        QTRY_VERIFY(fixture.controller.replyTabs().isEmpty());

        // The sendable property re-notifies when a tab close removes the last
        // saved answer while the active draft stays blank (canSend NOTIFY
        // stateChanged must cover per-tab answers, not only the draft).
        QVERIFY(fixture.controller.beginReply(firstId));
        fixture.controller.setDraft("saved answer");
        QVERIFY(fixture.controller.addReplyTab(secondId)); // active tab stays blank
        QVERIFY(fixture.controller.canSend());
        QSignalSpy sendable(&fixture.controller, &NativeController::stateChanged);
        fixture.controller.removeReplyTab(fixture.controller.replyTabs()[0].toMap().value("id").toString());
        QVERIFY(!fixture.controller.canSend());
        QVERIFY(sendable.count() > 0);
        fixture.controller.cancelReply();
    }

    // The send constraints cover every answer in the composed message, not
    // just the active tab: an oversized answer saved in an INACTIVE tab must
    // not slip past the size limit that gates the active draft. The
    // anyReplyAnswer gate makes the inactive answer sendable, so the same
    // draft().size() <= 40000 rule must be applied to it at canSend time.
    void multiReplyTabsRejectOversizedInactiveAnswer()
    {
        Fixture fixture;
        const QString peerA = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        const QString peerB = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerA}, {"username", "amy"}, {"first_name", "Amy"}},
            QJsonObject{{"uuid", peerB}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString firstId = "11111111-1111-4111-8111-111111111111";
        const QString secondId = "22222222-2222-4222-8222-222222222222";
        auto noteA = message("Alpha note", firstId);
        noteA["is_own"] = false; noteA["author_uuid"] = peerA;
        auto noteB = message("Beta note", secondId);
        noteB["is_own"] = false; noteB["author_uuid"] = peerB;
        int posts = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++posts; return {QJsonArray{}, 500}; }
            if (request.method == "GET" && request.target.path().endsWith("/messages/"))
                return {QJsonArray{noteA, noteB}};
            return {QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.beginReply(firstId));
        fixture.controller.setDraft(QString(40001, QLatin1Char('x'))); // oversized while active
        QVERIFY(!fixture.controller.canSend());
        QVERIFY(fixture.controller.addReplyTab(secondId)); // answer moves into inactive tab A
        QVERIFY(fixture.controller.draft().isEmpty());
        QVERIFY(!fixture.controller.canSend()); // inactive answer must not bypass the size limit
        fixture.controller.send();
        QCOMPARE(posts, 0);
        // The limit itself does not move: exactly 40000 in an inactive tab is
        // sendable, matching the single-draft rule (draft().size() <= 40000).
        const QString tabA = fixture.controller.replyTabs()[0].toMap().value("id").toString();
        const QString tabB = fixture.controller.replyTabs()[1].toMap().value("id").toString();
        QVERIFY(fixture.controller.selectReplyTab(tabA));
        QCOMPARE(fixture.controller.draft().size(), qsizetype(40001));
        fixture.controller.setDraft(QString(40000, QLatin1Char('x')));
        QVERIFY(fixture.controller.selectReplyTab(tabB)); // blank active tab, inactive answer at the limit
        QVERIFY(fixture.controller.canSend());
        fixture.controller.cancelReply();
    }

    // Same gate, protected-fragment rule: ":::" text saved in an inactive tab
    // must not reach the wire just because the active draft is clean (the
    // active-draft rule is protectedFragmentsAreNotSentAsOrdinaryPlaintext).
    void multiReplyTabsRejectProtectedFragmentInInactiveAnswer()
    {
        Fixture fixture;
        const QString peerA = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        const QString peerB = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerA}, {"username", "amy"}, {"first_name", "Amy"}},
            QJsonObject{{"uuid", peerB}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString firstId = "11111111-1111-4111-8111-111111111111";
        const QString secondId = "22222222-2222-4222-8222-222222222222";
        auto noteA = message("Alpha note", firstId);
        noteA["is_own"] = false; noteA["author_uuid"] = peerA;
        auto noteB = message("Beta note", secondId);
        noteB["is_own"] = false; noteB["author_uuid"] = peerB;
        int posts = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++posts; return {QJsonArray{}, 500}; }
            if (request.method == "GET" && request.target.path().endsWith("/messages/"))
                return {QJsonArray{noteA, noteB}};
            return {QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.beginReply(firstId));
        fixture.controller.setDraft(":::test-only protected fragment:::"); // blocked while active
        QVERIFY(!fixture.controller.canSend());
        QVERIFY(fixture.controller.addReplyTab(secondId)); // fragment moves into inactive tab A
        QVERIFY(fixture.controller.draft().isEmpty());
        QVERIFY(!fixture.controller.canSend()); // inactive answer must not bypass the content rule
        fixture.controller.send();
        QCOMPARE(posts, 0);
        fixture.controller.cancelReply();
    }

    // Tab store rules ported from workspace-reply.lib.ts: select, close with the
    // active-tab fallback, reorder bounds, and the draft lockstep with the
    // active tab's answer.
    void multiReplyTabCloseSelectAndReorderSemantics()
    {
        Fixture fixture;
        const QString peerA = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        const QString peerB = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerA}, {"username", "amy"}, {"first_name", "Amy"}},
            QJsonObject{{"uuid", peerB}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString firstId = "11111111-1111-4111-8111-111111111111";
        const QString secondId = "22222222-2222-4222-8222-222222222222";
        auto noteA = message("Alpha note", firstId);
        noteA["is_own"] = false; noteA["author_uuid"] = peerA;
        auto noteB = message("Beta note", secondId);
        noteB["is_own"] = false; noteB["author_uuid"] = peerB;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path().endsWith("/messages/")) return {QJsonArray{noteA, noteB}};
            return {QJsonArray{}};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginReply(firstId));
        fixture.controller.setDraft("answer one");
        QVERIFY(fixture.controller.addReplyTab(secondId));
        fixture.controller.setDraft("answer two");
        const QString tabA = fixture.controller.replyTabs()[0].toMap().value("id").toString();
        const QString tabB = fixture.controller.replyTabs()[1].toMap().value("id").toString();

        // Unknown ids are refused without touching the session.
        QVERIFY(!fixture.controller.addReplyTab("00000000-0000-4000-8000-000000000000"));
        QVERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.replyTabs().size(), 2);
        QVERIFY(!fixture.controller.selectReplyTab("missing-tab"));
        QVERIFY(!fixture.controller.reorderReplyTab("missing-tab", 0));
        QCOMPARE(fixture.controller.replyTabs().size(), 2);

        // beginReply on an open session replaces only the active tab's quote
        // (replyToWorkspaceReply keeps the tab id and answer).
        QVERIFY(fixture.controller.beginReply(firstId));
        QCOMPARE(fixture.controller.replyTabs().size(), 2);
        QCOMPARE(fixture.controller.replyTabs()[1].toMap().value("message").toString(), firstId);
        QCOMPARE(fixture.controller.replyTabs()[1].toMap().value("id").toString(), tabB);
        QCOMPARE(fixture.controller.draft(), QString("answer two"));
        // Restore the two-quote session for the remaining assertions.
        QVERIFY(fixture.controller.selectReplyTab(tabA));
        fixture.controller.beginReply(firstId);
        fixture.controller.selectReplyTab(tabB);
        fixture.controller.beginReply(secondId);
        fixture.controller.setDraft("answer two");

        // Selecting a tab swaps the composer to that tab's answer.
        QVERIFY(fixture.controller.selectReplyTab(tabA));
        QCOMPARE(fixture.controller.draft(), QString("answer one"));
        QCOMPARE(fixture.controller.replyMessageId(), firstId);
        QVERIFY(fixture.controller.selectReplyTab(tabB));
        QCOMPARE(fixture.controller.draft(), QString("answer two"));

        // Reorder bounds: destination is an insertion point clamped to [0, count];
        // a no-op move reports false (workspace-reply.lib.ts:159-192).
        QVERIFY(!fixture.controller.reorderReplyTab(tabB, 1)); // already inserted at 1
        QVERIFY(fixture.controller.reorderReplyTab(tabB, 0)); // [B, A]
        QVERIFY(!fixture.controller.reorderReplyTab(tabA, 1)); // [B, A] insertion at 1 keeps [B, A]
        QVERIFY(fixture.controller.reorderReplyTab(tabB, 99)); // clamps to the end: [A, B]
        QVERIFY(!fixture.controller.reorderReplyTab(tabB, 2)); // already last
        QCOMPARE(fixture.controller.replyTabs()[1].toMap().value("id").toString(), tabB);

        // Closing the active tab falls back to the following tab, then the previous.
        fixture.controller.removeReplyTab(tabB);
        QCOMPARE(fixture.controller.replyTabs().size(), 1);
        QCOMPARE(fixture.controller.replyMessageId(), firstId);
        QCOMPARE(fixture.controller.draft(), QString("answer one"));
        // Closing the last tab empties the session and leaves the answer in the
        // composer (chat-page-workspace.ui.tsx:3030-3041).
        fixture.controller.setDraft("typed again");
        fixture.controller.removeReplyTab(tabA);
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("typed again"));
        fixture.controller.removeReplyTab(tabA); // closing on an empty session is a no-op
        QVERIFY(fixture.controller.replyTabs().isEmpty());
    }

    // Every tab's answer gets composer-mention serialization at send time, not
    // just the active draft (message-composer-body.lib.ts buildOutgoingMessageBody:
    // mentions serialize wherever an answer block is composed).
    void multiReplyTabsSerializeMentionsInEveryAnswer()
    {
        Fixture f;
        const auto bob = mentionUuid("0b0b"), ann = mentionUuid("0a55");
        f.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "cassi"}},
            QJsonObject{{"uuid", bob}, {"username", "bob"}, {"first_name", "Bob"}, {"last_name", "Ray"}},
            QJsonObject{{"uuid", ann}, {"username", "ann"}, {"first_name", "Ann"}}}}; };
        const QString firstId = "89000000-0000-4000-8000-000000000001";
        const QString secondId = "89000000-0000-4000-8000-000000000002";
        auto noteOne = message(QStringLiteral("First peer note"), firstId);
        noteOne["author_uuid"] = ann; noteOne["is_own"] = false;
        auto noteTwo = message(QStringLiteral("Second peer note"), secondId);
        noteTwo["author_uuid"] = bob; noteTwo["is_own"] = false;
        QJsonObject posted, saved;
        f.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") {
                posted = request.body;
                saved = message(request.body.value("payload").toObject()
                    .value("content").toString(), QStringLiteral("89000000-0000-4000-8000-000000000003"));
                saved["user_uuid"] = userId;
                return Response{saved, 201};
            }
            if (request.method == "GET" && request.target.path()
                .endsWith(QStringLiteral("89000000-0000-4000-8000-000000000003")) && !saved.isEmpty())
                return Response{saved};
            return Response{QJsonArray{noteOne, noteTwo}};
        };
        QVERIFY(f.start());
        QVERIFY(f.controller.beginReply(firstId));
        f.controller.setDraft(QStringLiteral("Hi @Bob Ray!")); // answer captured by tab 1
        QVERIFY(f.controller.addReplyTab(secondId));           // tab 2 activates blank
        f.controller.setDraft(QStringLiteral("cc @Ann"));
        QVERIFY(f.controller.canSend());
        f.controller.send();
        QTRY_VERIFY_WITH_TIMEOUT(!posted.isEmpty(), 4000);
        const auto content = posted.value("payload").toObject().value("content").toString();
        // Tab 1 is inactive when the send composes: its saved "@DisplayName" text
        // must still reach the wire as the canonical urn:user mention.
        QCOMPARE(content, QStringLiteral("[Ann](urn:quote:%1)\n\nHi [Bob Ray](urn:user:%2)!\n\n"
            "[Bob Ray](urn:quote:%3)\n\ncc [Ann](urn:user:%4)").arg(firstId, bob, secondId, ann));
    }

    // A confirmed send must not close a session whose quote the user re-pointed
    // while that send was in flight: beginReply can replace a tab's quoted
    // message without changing the tab ids or the active pointer.
    void multiReplyConfirmedSendKeepsSessionWhoseQuoteChangedInFlight()
    {
        Fixture fixture;
        const QString peerA = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        const QString peerB = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        const QString peerC = "7c7c7c7c-7c7c-4c7c-8c7c-7c7c7c7c7c7c";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerA}, {"username", "amy"}, {"first_name", "Amy"}},
            QJsonObject{{"uuid", peerB}, {"username", "bobby"}, {"first_name", "Bobby"}},
            QJsonObject{{"uuid", peerC}, {"username", "cleo"}, {"first_name", "Cleo"}}}}; };
        const QString firstId = "11111111-1111-4111-8111-111111111111";
        const QString secondId = "22222222-2222-4222-8222-222222222222";
        const QString thirdId = "33333333-3333-4333-8333-333333333333";
        auto noteA = message("Alpha note", firstId);
        noteA["is_own"] = false; noteA["author_uuid"] = peerA;
        auto noteB = message("Beta note", secondId);
        noteB["is_own"] = false; noteB["author_uuid"] = peerB;
        auto noteC = message("Gamma note", thirdId);
        noteC["is_own"] = false; noteC["author_uuid"] = peerC;
        QJsonObject saved; std::function<void()> confirm;
        const auto release = qScopeGuard([&] { if (confirm) confirm(); });
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") {
                saved = message(request.body.value("payload").toObject()
                    .value("content").toString(), returnedId);
                return Response{saved, 201};
            }
            if (request.method == "GET" && request.target.path().endsWith(returnedId)) {
                Response response{saved};
                response.defer = [&](std::function<void()> send) { confirm = std::move(send); };
                return response;
            }
            if (request.method == "GET" && request.target.path().endsWith("/messages/"))
                return Response{QJsonArray{noteA, noteB, noteC}};
            return Response{saved};
        };
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 3);
        QVERIFY(fixture.controller.beginReply(firstId));
        fixture.controller.setDraft("answer one");
        QVERIFY(fixture.controller.addReplyTab(secondId));
        fixture.controller.setDraft("answer two");
        fixture.controller.send();
        QTRY_VERIFY(bool(confirm));
        // While the send is being verified the user re-points the active tab at a
        // different quote; the tab ids and the active pointer do not change.
        QVERIFY(fixture.controller.beginReply(thirdId));
        QCOMPARE(fixture.controller.replyTabs().size(), 2);
        confirm(); confirm = {};
        QTRY_VERIFY(!fixture.controller.busy());
        // The session the user just changed must survive the confirmed send.
        QCOMPARE(fixture.controller.replyTabs().size(), 2);
        const auto tabs = fixture.controller.replyTabs();
        QCOMPARE(tabs[0].toMap().value("message").toString(), firstId);
        QCOMPARE(tabs[1].toMap().value("message").toString(), thirdId);
        QCOMPARE(fixture.controller.replyMessageId(), thirdId);
    }

    void sendConfirmationPreservesInFlightReplyAnswers_data()
    {
        QTest::addColumn<bool>("durable");
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("parked");
        QTest::addColumn<bool>("holdReadBack");
        for (const bool durable : {false, true}) {
            const auto prefix = durable ? QByteArray("durable-") : QByteArray("memory-");
            for (const auto *change : {"unchanged", "active", "inactive", "reverted", "cleared",
                                      "plain-unchanged", "plain-active", "plain-reverted"})
                QTest::newRow((prefix + change).constData()) << durable << QString(change) << false << false;
            QTest::newRow((prefix + "inactive-parked").constData()) << durable << QString("inactive") << true << false;
            QTest::newRow((prefix + "unchanged-parked").constData()) << durable << QString("unchanged") << true << false;
            QTest::newRow((prefix + "active-readback").constData()) << durable << QString("active") << false << true;
        }
    }

    void sendConfirmationPreservesInFlightReplyAnswers()
    {
        QFETCH(bool, durable); QFETCH(QString, change); QFETCH(bool, parked); QFETCH(bool, holdReadBack);
        QTemporaryDir directory; NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.extraTopic = true;
        if (durable) fixture.controller.useDrafts(&drafts);
        const QString otherId = "23232323-2323-4323-8323-232323232323";
        auto first = message("First origin", messageId);
        auto second = message("Second origin", secondTopicId);
        auto other = message("Other conversation", otherId, secondTopicId);
        QJsonObject saved; int posts = 0;
        std::function<void()> complete;
        const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") {
                ++posts;
                saved = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                Response response{saved, 201};
                if (!holdReadBack) response.defer = [&](auto send) { complete = std::move(send); };
                return response;
            }
            if (request.target.path().endsWith(returnedId)) {
                Response response{saved};
                if (holdReadBack) response.defer = [&](auto send) { complete = std::move(send); };
                return response;
            }
            return {QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId
                ? QJsonArray{other} : QJsonArray{first, second}};
        };
        QVERIFY(fixture.start());
        if (durable) QTRY_VERIFY(drafts.ready(streamId, topicId));
        const bool reply = !change.startsWith("plain-");
        if (reply) {
            QVERIFY(fixture.controller.beginReply(messageId));
            fixture.controller.setDraft("first");
            QVERIFY(fixture.controller.addReplyTab(secondTopicId));
        }
        fixture.controller.setDraft("second");
        const auto originalTabs = fixture.controller.replyTabs();
        fixture.controller.send();
        QTRY_VERIFY(bool(complete)); QCOMPARE(posts, 1);
        const auto wire = saved.value("payload").toObject().value("content").toString();
        if (reply) {
            QVERIFY(wire.contains("urn:quote:" + messageId));
            QVERIFY(wire.contains("\n\nfirst\n\n"));
            QVERIFY(wire.endsWith("\n\nsecond"));
        } else QCOMPARE(wire, QString("second"));

        QString expectedText = "second";
        if (change == "active" || change == "plain-active") {
            expectedText = "second edited"; fixture.controller.setDraft(expectedText);
        } else if (change == "inactive") {
            QVERIFY(fixture.controller.selectReplyTab(originalTabs[0].toMap().value("id").toString()));
            fixture.controller.setDraft("first edited");
            QVERIFY(fixture.controller.selectReplyTab(originalTabs[1].toMap().value("id").toString()));
        } else if (change.endsWith("reverted")) {
            fixture.controller.setDraft("second edited"); fixture.controller.setDraft("second");
        } else if (change == "cleared") {
            expectedText.clear(); fixture.controller.setDraft(expectedText);
        }
        const auto expectedTabs = fixture.controller.replyTabs();
        const bool changed = !change.endsWith("unchanged");
        if (parked) {
            fixture.controller.selectTopic(secondTopicId);
            QTRY_COMPARE(fixture.controller.messages().size(), 1);
            if (durable) QTRY_VERIFY(drafts.ready(streamId, secondTopicId));
            QVERIFY(fixture.controller.beginReply(otherId));
            fixture.controller.setDraft("unrelated answer");
        }
        complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(posts, 1); QVERIFY(fixture.controller.error().isEmpty());
        if (parked) {
            QCOMPARE(fixture.controller.replyMessageId(), otherId);
            QCOMPARE(fixture.controller.draft(), QString("unrelated answer"));
            fixture.controller.selectTopic(topicId);
            QTRY_VERIFY(!fixture.controller.busy());
            if (durable) QTRY_VERIFY(drafts.ready(streamId, topicId));
        }
        // The durable store's token comparison already protects newer composer
        // text. The reply close guard must preserve every answer alongside it.
        QCOMPARE(fixture.controller.draft(), changed ? expectedText : QString());
        QCOMPARE(fixture.controller.replyTabs().size(), reply && changed ? 2 : 0);
        if (reply && changed) {
            const auto tabs = fixture.controller.replyTabs();
            for (int index = 0; index < tabs.size(); ++index) {
                for (const auto *field : {"id", "message", "answer", "active"})
                    QCOMPARE(tabs[index].toMap().value(field), expectedTabs[index].toMap().value(field));
            }
        }
        if (durable) {
            const auto owner = drafts.owner();
            bool flushed = false; drafts.flush(this, [&](bool okay) { flushed = okay; }); QTRY_VERIFY(flushed);
            storage.shutdown(); QTRY_VERIFY(storage.stopped());
            NativeDraftStore store(directory.path()); QCOMPARE(store.open(owner), NativeDraftService::Failure::None);
            const auto latest = store.latest(streamId, topicId);
            if (changed && !expectedText.isEmpty()) {
                QVERIFY(latest.record); QCOMPARE(latest.record->text, expectedText);
            } else QVERIFY(!latest.record);
        }
    }

    // ---- per-conversation reply session store (issue #201) ----

    // One stream, two topics: conversation A is `topicId`, conversation B is
    // `secondTopicId`. Each topic's history serves only its own messages, so a
    // restored banner that still resolves its author proves which conversation
    // is loaded. docs/NATIVE_DESKTOP_REPLY_SESSION_PERSISTENCE.md.
    static void replyPersistenceScene(Fixture &fixture)
    {
        fixture.extraTopic = true;
        const QString peerA = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        const QString peerB = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        fixture.users = [peerA, peerB](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerA}, {"username", "amy"}, {"first_name", "Amy"}},
            QJsonObject{{"uuid", peerB}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        auto first = message("Alpha note", QStringLiteral("11111111-1111-4111-8111-111111111111"));
        first["is_own"] = false; first["author_uuid"] = peerA;
        auto second = message("Alpha second note", QStringLiteral("12121212-1212-4212-8212-121212121212"));
        second["is_own"] = false; second["author_uuid"] = peerB;
        auto beta = message("Beta note", QStringLiteral("23232323-2323-4323-8323-232323232323"), secondTopicId);
        beta["is_own"] = false; beta["author_uuid"] = peerB;
        fixture.messages = [first, second, beta](const Request &request) -> Response {
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{beta}};
            return Response{QJsonArray{first, second}};
        };
    }

    void replySessionSurvivesNavigationAwayAndBack()
    {
        Fixture fixture; replyPersistenceScene(fixture);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.beginReply(QStringLiteral("11111111-1111-4111-8111-111111111111")));
        fixture.controller.setDraft("answer alpha");
        QCOMPARE(fixture.controller.replyAuthor(), QString("Amy"));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Alpha note"));
        const auto tabsBefore = fixture.controller.replyTabs();
        QSignalSpy changed(&fixture.controller, &NativeController::replySessionChanged);
        QVERIFY(changed.isValid());

        // Conversation B opens with its own empty session; A's parked session
        // stays invisible while B is current.
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QVERIFY(fixture.controller.replyAuthor().isEmpty());

        // Returning to A restores the whole session unchanged: tab identity,
        // quote target, author, excerpt and the composer answer.
        fixture.controller.selectTopic(topicId);
        QTRY_COMPARE(fixture.controller.replyMessageId(),
                     QStringLiteral("11111111-1111-4111-8111-111111111111"));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.controller.replyAuthor(), QString("Amy"));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Alpha note"));
        QCOMPARE(fixture.controller.draft(), QString("answer alpha"));
        QVERIFY(fixture.controller.replyTabs() == tabsBefore);
        // Every parked/restore transition a QML binding reads is announced.
        QVERIFY(changed.count() > 0);
    }

    void replySessionCancelStaysCancelledAcrossNavigation()
    {
        Fixture fixture; replyPersistenceScene(fixture);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.beginReply(QStringLiteral("11111111-1111-4111-8111-111111111111")));
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        fixture.controller.selectTopic(topicId);
        QTRY_COMPARE(fixture.controller.replyMessageId(),
                     QStringLiteral("11111111-1111-4111-8111-111111111111"));
        // Cancelling drops the parked entry too, so the next revisit is empty.
        fixture.controller.cancelReply();
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        fixture.controller.selectTopic(topicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
    }

    void replySessionsAreIsolatedPerConversation()
    {
        Fixture fixture; replyPersistenceScene(fixture);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.beginReply(QStringLiteral("11111111-1111-4111-8111-111111111111")));
        fixture.controller.setDraft("answer alpha");
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QVERIFY(fixture.controller.beginReply(QStringLiteral("23232323-2323-4323-8323-232323232323")));
        fixture.controller.setDraft("beta answer");

        // Each conversation keeps its own session, answer included.
        fixture.controller.selectTopic(topicId);
        QTRY_COMPARE(fixture.controller.replyMessageId(),
                     QStringLiteral("11111111-1111-4111-8111-111111111111"));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.controller.draft(), QString("answer alpha"));
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.replyMessageId(),
                     QStringLiteral("23232323-2323-4323-8323-232323232323"));
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QCOMPARE(fixture.controller.replyAuthor(), QString("Bobby"));
        QCOMPARE(fixture.controller.draft(), QString("beta answer"));

        // Closing B's session never touches A's parked session, and B stays
        // cancelled on the next revisit.
        fixture.controller.cancelReply();
        fixture.controller.selectTopic(topicId);
        QTRY_COMPARE(fixture.controller.replyMessageId(),
                     QStringLiteral("11111111-1111-4111-8111-111111111111"));
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QVERIFY(fixture.controller.replyTabs().isEmpty());
    }

    void multiTabReplySessionSurvivesNavigation()
    {
        Fixture fixture; replyPersistenceScene(fixture);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.beginReply(QStringLiteral("11111111-1111-4111-8111-111111111111")));
        fixture.controller.setDraft("answer one");
        QVERIFY(fixture.controller.addReplyTab(QStringLiteral("12121212-1212-4212-8212-121212121212")));
        fixture.controller.setDraft("answer two");
        const QString firstTab = fixture.controller.replyTabs()[0].toMap().value("id").toString();
        QVERIFY(fixture.controller.reorderReplyTab(
            fixture.controller.replyTabs()[1].toMap().value("id").toString(), 0));
        QCOMPARE(fixture.controller.replyMessageId(),
                 QStringLiteral("12121212-1212-4212-8212-121212121212"));
        const auto tabsBefore = fixture.controller.replyTabs();

        // The whole session - tab set, order, active pointer and every answer -
        // is parked together (docs/NATIVE_DESKTOP_MULTI_REPLY_TABS.md state).
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        fixture.controller.selectTopic(topicId);
        QTRY_COMPARE(fixture.controller.replyMessageId(),
                     QStringLiteral("12121212-1212-4212-8212-121212121212"));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.replyTabs() == tabsBefore);
        QCOMPARE(fixture.controller.replyTabs()[0].toMap().value("id").toString() != firstTab, true);
        QCOMPARE(fixture.controller.draft(), QString("answer two"));
        QCOMPARE(fixture.controller.replyAuthor(), QString("Bobby"));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Alpha second note"));
    }

    void parkedReplySessionKeepsAuthorAndExcerptWhileHistoryLoads()
    {
        Fixture fixture; replyPersistenceScene(fixture);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.beginReply(QStringLiteral("11111111-1111-4111-8111-111111111111")));
        fixture.controller.setDraft("answer alpha");
        // No display getter (replyAuthor/replyExcerpt/replyTabs row) runs
        // before leaving, so the park must prime the memos from the still
        // loaded message itself (review #246, native-controller.cpp:11055).
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        fixture.controller.selectTopic(topicId);
        // selectTopic commits the route synchronously: the restored tab is
        // live right now and A's history has not arrived yet, so only a memo
        // primed at park time can render the banner.
        QCOMPARE(fixture.controller.replyMessageId(),
                 QStringLiteral("11111111-1111-4111-8111-111111111111"));
        QCOMPARE(fixture.controller.replyAuthor(), QString("Amy"));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Alpha note"));
        // Once history returns, live resolution still wins over the memo.
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.controller.replyAuthor(), QString("Amy"));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Alpha note"));
    }

    void liveEmptyExcerptReplacesParkedMemo()
    {
        // A present message whose rendered plain excerpt is empty (its body
        // became whitespace-only / media-only after an edit) is live content,
        // not a lookup miss: the banner must show the current empty excerpt,
        // never the memo left behind by the earlier text body. The tab label
        // then falls back to the bare author name exactly as the reference
        // getTabLabel does for an empty excerpt (workspace_ui 61dacef
        // workspace-reply.ui.tsx).
        Fixture fixture; replyPersistenceScene(fixture);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.beginReply(QStringLiteral("11111111-1111-4111-8111-111111111111")));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Alpha note"));

        // Park to conversation B, then edit the quoted message on the server
        // to a body that renders no plain text before coming back to A.
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        auto blank = message(QStringLiteral("   "),
                             QStringLiteral("11111111-1111-4111-8111-111111111111"));
        blank["is_own"] = false;
        blank["author_uuid"] = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        auto second = message(QStringLiteral("Alpha second note"),
                              QStringLiteral("12121212-1212-4212-8212-121212121212"));
        second["is_own"] = false;
        second["author_uuid"] = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        auto beta = message(QStringLiteral("Beta note"),
                            QStringLiteral("23232323-2323-4323-8323-232323232323"), secondTopicId);
        beta["is_own"] = false;
        beta["author_uuid"] = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        fixture.messages = [blank, second, beta](const Request &request) -> Response {
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{beta}};
            return Response{QJsonArray{blank, second}};
        };
        fixture.controller.selectTopic(topicId);
        // Restored live with A's history still loading: the memo is the only
        // renderable source, so the banner may show it for now.
        QCOMPARE(fixture.controller.replyMessageId(),
                 QStringLiteral("11111111-1111-4111-8111-111111111111"));
        QCOMPARE(fixture.controller.replyAuthor(), QString("Amy"));
        // History arrives: the message is PRESENT with an empty rendered
        // excerpt, so live resolution (empty) must win over the stale memo.
        QTRY_COMPARE(fixture.controller.replyExcerpt(), QString());
        const auto tabs = fixture.controller.replyTabs();
        QCOMPARE(tabs.size(), 1);
        QCOMPARE(tabs[0].toMap().value("excerpt").toString(), QString());
        QCOMPARE(tabs[0].toMap().value("label").toString(), QString("Amy"));
        // The author stays live-resolvable from the present message.
        QCOMPARE(fixture.controller.replyAuthor(), QString("Amy"));
    }

    // Issue #202 (docs/NATIVE_DESKTOP_EDIT_REPLY_RESTORE.md): opening a stored
    // message whose body starts with "[Name](urn:quote:...)" restores the reply
    // session and edits only the answer; re-saving round-trips the quote line.
    void inlineEditRestoresQuoteReplyAndPreservesTheQuoteLineOnSave()
    {
        Fixture fixture;
        auto origin = message("Origin text", messageId);
        auto note = message(QStringLiteral("[You](urn:quote:%1?text=kept%20selection)\n\nmy answer").arg(messageId), returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        QJsonObject saved; int edits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{origin, note}};
            if (request.method == "PUT") { ++edits; saved = request.body; note["payload"] = request.body.value("payload"); return {note}; }
            return {path.endsWith(messageId) ? origin : note};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        // The quote reference is taken off the body: the composer shows only the answer.
        QCOMPARE(fixture.controller.draft(), QString("my answer"));
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
        const auto tabs = fixture.controller.replyTabs();
        QCOMPARE(tabs.size(), 1);
        QCOMPARE(tabs.first().toMap().value("message").toString(), messageId);
        QVERIFY(tabs.first().toMap().value("active").toBool());
        QVERIFY(fixture.controller.canSaveEdit());
        QVERIFY(!fixture.controller.canSend());

        // The restored tab answers in lockstep with the edit buffer.
        fixture.controller.setDraft("fixed answer");
        QCOMPARE(fixture.controller.replyTabs().first().toMap().value("answer").toString(), QString("fixed answer"));

        // Re-saving preserves the quote line verbatim, including the ?text= fragment.
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 1);
        QCOMPARE(saved.value("payload").toObject().value("content").toString(),
                 QStringLiteral("[You](urn:quote:%1?text=kept%20selection)\n\nfixed answer").arg(messageId));
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
    }

    void inlineEditRestoreCancelClosesTheRestoredReplySession()
    {
        Fixture fixture;
        auto origin = message("Origin text", messageId);
        auto note = message(QStringLiteral("[You](urn:quote:%1)\n\nmy answer").arg(messageId), returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        int edits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{origin, note}};
            if (request.method == "PUT") { ++edits; return {note}; }
            return {path.endsWith(messageId) ? origin : note};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("pre-edit draft");

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.replyTabs().size(), 1);
        fixture.controller.cancelEditMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        // The restored tabs were session-local: cancelling closes them too and
        // the stored draft is back.
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("pre-edit draft"));
        QCOMPARE(edits, 0);
    }

    void inlineEditRestoreSplitsMultipleQuoteBlocksIntoTabs()
    {
        Fixture fixture;
        const QString secondOriginId = "7b7b7b7b-7b7b-4b7b-8b7b-7b7b7b7b7b7b";
        auto firstOrigin = message("First origin", messageId);
        auto secondOrigin = message("Second origin", secondOriginId);
        auto note = message(QStringLiteral("[First](urn:quote:%1)\n\nfirst answer\n\n[Second](urn:quote:%2)\n\nsecond answer")
            .arg(messageId, secondOriginId), returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        QJsonObject saved; int edits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{firstOrigin, secondOrigin, note}};
            if (request.method == "PUT") { ++edits; saved = request.body; note["payload"] = request.body.value("payload"); return {note}; }
            return {path.endsWith(messageId) ? firstOrigin
                : path.endsWith(secondOriginId) ? secondOrigin : note};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto tabs = fixture.controller.replyTabs();
        QCOMPARE(tabs.size(), 2);
        QCOMPARE(tabs[0].toMap().value("message").toString(), messageId);
        QCOMPARE(tabs[1].toMap().value("message").toString(), secondOriginId);
        QCOMPARE(fixture.controller.draft(), QString("first answer"));

        // Switching tabs swaps the edit buffer to that tab's answer.
        QVERIFY(fixture.controller.selectReplyTab(tabs[1].toMap().value("id").toString()));
        QCOMPARE(fixture.controller.draft(), QString("second answer"));
        fixture.controller.setDraft("second answer, edited");

        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 1);
        QCOMPARE(saved.value("payload").toObject().value("content").toString(),
                 QStringLiteral("[First](urn:quote:%1)\n\nfirst answer\n\n[Second](urn:quote:%2)\n\nsecond answer, edited")
                     .arg(messageId, secondOriginId));
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QVERIFY(fixture.controller.replyTabs().isEmpty());
    }

    void inlineEditLeavesNonStructuralQuoteMentionsRaw()
    {
        Fixture fixture;
        auto origin = message("Origin text", messageId);
        auto inlineRef = message(QStringLiteral("See [You](urn:quote:%1) inline.").arg(messageId), returnedId);
        inlineRef["created_at"] = "2026-01-02T10:00:01.000Z";
        const QString codeId = "5c5c5c5c-5c5c-4c5c-8c5c-5c5c5c5c5c5c";
        auto codeRef = message(QStringLiteral("Use `[You](urn:quote:%1)` as code.").arg(messageId), codeId);
        codeRef["created_at"] = "2026-01-02T10:00:02.000Z";
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{origin, inlineRef, codeRef}};
            return {path.endsWith(messageId) ? origin : path.endsWith(returnedId) ? inlineRef : codeRef};
        };
        QVERIFY(fixture.start());

        // An inline occurrence is not a structural quote: the body edits raw.
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), inlineRef.value("payload").toObject().value("content").toString());
        fixture.controller.cancelEditMessage();

        QVERIFY(fixture.controller.beginEditMessage(codeId));
        QVERIFY(fixture.controller.replyTabs().isEmpty());
        QCOMPARE(fixture.controller.draft(), codeRef.value("payload").toObject().value("content").toString());
        fixture.controller.cancelEditMessage();
    }

    void inlineEditRestoreRecognizesTheLegacyBlockquoteHeader()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        auto origin = message("Origin text", messageId);
        origin["is_own"] = false; origin["author_uuid"] = peerId;
        auto note = message(QStringLiteral("> [Bobby](urn:user:%1) [wrote](urn:message:%2):\n> Origin text\n\nmy answer")
            .arg(peerId, messageId), returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        QJsonObject saved; int edits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{origin, note}};
            if (request.method == "PUT") { ++edits; saved = request.body; note["payload"] = request.body.value("payload"); return {note}; }
            return {path.endsWith(messageId) ? origin : note};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.replyTabs().size(), 1);
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
        QCOMPARE(fixture.controller.draft(), QString("my answer"));

        // The legacy header round-trips to the current urn:quote form (the tab
        // serializer always emits the reference form, as the reference does).
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 1);
        QCOMPARE(saved.value("payload").toObject().value("content").toString(),
                 QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(messageId));
    }

    void inlineEditRestoreFetchesQuoteOriginsOutsideTheWindow()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString far = "4d4d4d4d-4d4d-4d4d-8d4d-4d4d4d4d4d4d";
        auto farOrigin = message("Far body", far);
        farOrigin["is_own"] = false; farOrigin["author_uuid"] = peerId;
        auto note = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(far), returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        int lookups = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) {
                ++lookups;
                return query.queryItemValue("uuid") == far ? Response{QJsonArray{farOrigin}} : Response{QJsonArray{}};
            }
            return Response{QJsonArray{note}};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        // The tab restores synchronously with the link label as the fallback
        // name; the quote lookup fills the chip once it lands (once only).
        QCOMPARE(fixture.controller.replyMessageId(), far);
        QTRY_COMPARE(fixture.controller.replyExcerpt(), QString("Far body"));
        QCOMPARE(fixture.controller.replyAuthor(), QString("Bobby"));
        QTRY_COMPARE(lookups, 1);
    }

    // Review thread 4559371966: a quote-like line inside a fenced code block is
    // code text, never a structural quote. The body starts with one real quote
    // and the answer carries a fenced block with a standalone urn:quote line.
    void inlineEditRestoreIgnoresQuoteLinesInsideFencedCode()
    {
        Fixture fixture;
        const QString otherId = "8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e";
        auto origin = message("Origin text", messageId);
        auto otherOrigin = message("Other origin", otherId);
        const QString fenced = QStringLiteral(
            "[You](urn:quote:%1)\n\nmy answer\n\n```\ninside code\n\n[Someone](urn:quote:%2)\n\nstill inside\n```\n\nafter fence")
            .arg(messageId, otherId);
        auto note = message(fenced, returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        QJsonObject saved; int edits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{origin, otherOrigin, note}};
            if (request.method == "PUT") { ++edits; saved = request.body; note["payload"] = request.body.value("payload"); return {note}; }
            return {path.endsWith(messageId) ? origin : path.endsWith(otherId) ? otherOrigin : note};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        // One tab: the fenced urn:quote line stays inside the answer verbatim.
        QCOMPARE(fixture.controller.replyTabs().size(), 1);
        QCOMPARE(fixture.controller.draft(), QStringLiteral(
            "my answer\n\n```\ninside code\n\n[Someone](urn:quote:%1)\n\nstill inside\n```\n\nafter fence").arg(otherId));
        QVERIFY(fixture.controller.canSaveEdit());

        // Re-saving round-trips the fenced answer verbatim (no re-splitting).
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 1);
        QCOMPARE(saved.value("payload").toObject().value("content").toString(), fenced);
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QVERIFY(fixture.controller.replyTabs().isEmpty());
    }

    // Review thread 4559371974: while a restored quote's origin is still
    // pending - or turns out to be unavailable - the chip keeps the label
    // parsed from the quote line instead of showing an empty author.
    void inlineEditRestoredQuoteShowsTheParsedLabelWhileTheOriginIsPending()
    {
        // Outlives the deferred send closure (declared before the fixture).
        std::function<void()> releaseLookup;
        Fixture fixture; fixture.aggregateMessages = true;
        const QString far = "4d4d4d4d-4d4d-4d4d-8d4d-4d4d4d4d4d4d";
        auto note = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(far), returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET" && QUrlQuery(request.target).hasQueryItem("uuid")) {
                // Authoritative miss, held in flight until the test releases it.
                Response response{QJsonArray{}};
                response.defer = [&releaseLookup](std::function<void()> send) { releaseLookup = std::move(send); };
                return response;
            }
            return Response{QJsonArray{note}};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.replyMessageId(), far);
        // The lookup has not been answered yet: chip and tab label answer with
        // the label parsed from the quote line, never an empty author.
        QCOMPARE(fixture.controller.replyAuthor(), QString("Bobby"));
        QCOMPARE(fixture.controller.replyTabs().first().toMap().value("author").toString(), QString("Bobby"));
        QVERIFY(fixture.controller.replyExcerpt().isEmpty());

        QSignalSpy sessionChanged(&fixture.controller, &NativeController::replySessionChanged);
        QVERIFY(sessionChanged.isValid());
        QTRY_VERIFY(static_cast<bool>(releaseLookup)); // The GET is in flight and held.
        releaseLookup(); releaseLookup = {};
        QVERIFY(sessionChanged.wait(2000)); // The unavailable answer lands; the chip re-reads.
        // An unresolvable origin keeps the stored label rather than blanking it.
        QCOMPARE(fixture.controller.replyAuthor(), QString("Bobby"));
        QCOMPARE(fixture.controller.replyTabs().first().toMap().value("author").toString(), QString("Bobby"));
        QCOMPARE(fixture.controller.replyMessageId(), far);
    }

    // Review thread 4559371966 (round 2): data-driven code-block coverage for
    // the fence/indent variants a fenced answer can carry. Expectations follow
    // the reference block model measured with marked 18.0.5 (gfm+breaks - the
    // lexer parseWorkspaceMessageBody uses): a quote-like line the renderer
    // puts inside code never splits a tab, while a line the renderer leaves in
    // a paragraph splits like any other structural quote.
    void inlineEditRestoreIgnoresQuoteLinesInCodeVariants_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<QStringList>("quoteIds");
        QTest::addColumn<QString>("firstAnswer");
        const QString first = messageId;
        const QString second = QStringLiteral("8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e");
        const QString third = QStringLiteral("9d9d9d9d-9d9d-4d9d-8d9d-9d9d9d9d9d9d");
        const QString header = QStringLiteral("[You](urn:quote:%1)\n\nanswer\n\n").arg(first);
        const QString q2 = QStringLiteral("[Someone](urn:quote:%1)").arg(second);
        const QString q3 = QStringLiteral("[Other](urn:quote:%1)").arg(third);
        auto row = [&](const char *name, const QString &rest, const QStringList &ids,
                       const QString &answer = QString()) {
            QTest::newRow(name) << (header + rest) << ids
                                << (QStringLiteral("answer\n\n")
                                    + (answer.isNull() ? rest : answer)).trimmed();
        };
        row("fenceSimple", QStringLiteral("```\nbefore\n\n%1\n\nstill code\n```\n\nafter fence").arg(q2),
            {first});
        row("fenceTilde", QStringLiteral("~~~\n\n%1\n\nstill\n~~~\n\nafter").arg(q2), {first});
        row("fenceIndentThreeSpaces",
            QStringLiteral("   ```\n\n%1\n\n   ```\n\nafter").arg(q2), {first});
        // 4 leading spaces make the fence line indented code, so the quote line
        // below stays a structural paragraph.
        row("fenceIndentedFourSpaces",
            QStringLiteral("    ```\n\n%1\n\nafter").arg(q2), {first, second}, "    ```");
        row("fenceInListNoBlankLineBeforeQuote",
            QStringLiteral("- ```\n%1\n\n  ```\n\nafter").arg(q2), {first});
        // A list-item fence ends with its item: the quote line at column 0 after
        // the blank line is outside the code (measured with marked).
        row("fenceInListQuoteEndsTheItem",
            QStringLiteral("- item\n\n  ```\n\n%1\n\n  ```\n\nafter").arg(q2),
            {first, second}, "- item\n\n  ```");
        row("fenceInListIndentedQuote",
            QStringLiteral("- item\n\n  ```\n\n  %1\n\n  ```\n\nafter").arg(q2), {first});
        // A backtick fence whose info string contains backticks is not a fence.
        row("openingBacktickInfoWithBackticks",
            QStringLiteral("```x```\n\n%1\n\nafter").arg(q2), {first, second}, "```x```");
        // Only a bare closing fence closes a block: "``` text" keeps the rest
        // of the answer inside the code.
        row("closingBacktickFenceWithInfo",
            QStringLiteral("```\n\n%1\n\n``` text\n\n%2\n\nafter").arg(q2, q3), {first});
        row("closingFenceShorterThanOpening",
            QStringLiteral("````\n\n%1\n\n```\n\n%2\n\nafter").arg(q2, q3), {first});
        row("tildeNotClosedByBackticks",
            QStringLiteral("~~~\n\n%1\n\n```\n\n%2\n\nafter").arg(q2, q3), {first});
        row("tildeInfoMayContainBackticks",
            QStringLiteral("~~~ info with `ticks\n\n%1\n\n~~~\n\nafter").arg(q2), {first});
        row("closingTildeFenceWithInfo",
            QStringLiteral("~~~\n\n%1\n\n~~~ text\n\n%2\n\nafter").arg(q2, q3), {first});
        row("unterminatedFenceRunsToEnd",
            QStringLiteral("```\n\n%1\n\nnever closes").arg(q2), {first});
        row("indentedCodeFourSpaces", QStringLiteral("    %1\n\nafter").arg(q2), {first});
        row("indentedCodeTab", QStringLiteral("\t%1\n\nafter").arg(q2), {first});
        row("twoCharFenceIsNotCode", QStringLiteral("``\n\n%1\n\nafter").arg(q2),
            {first, second}, "``");
    }

    void inlineEditRestoreIgnoresQuoteLinesInCodeVariants()
    {
        QFETCH(QString, body);
        QFETCH(QStringList, quoteIds);
        QFETCH(QString, firstAnswer);
        Fixture fixture;
        const QString second = QStringLiteral("8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e");
        const QString third = QStringLiteral("9d9d9d9d-9d9d-4d9d-8d9d-9d9d9d9d9d9d");
        auto origin = message("Origin text", messageId);
        auto secondOrigin = message("Second origin", second);
        auto thirdOrigin = message("Third origin", third);
        auto note = message(body, returnedId);
        note["created_at"] = "2026-01-02T10:00:01.000Z";
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{origin, secondOrigin, thirdOrigin, note}};
            return {path.endsWith(messageId) ? origin
                : path.endsWith(second) ? secondOrigin
                : path.endsWith(third) ? thirdOrigin : note};
        };
        QVERIFY(fixture.start());

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        const auto tabs = fixture.controller.replyTabs();
        QStringList actualIds;
        for (const auto &tab : tabs) actualIds.append(tab.toMap().value("message").toString());
        QCOMPARE(actualIds, quoteIds);
        if (!tabs.isEmpty())
            QCOMPARE(fixture.controller.draft(), firstAnswer);
    }

    void quoteReferenceRowsResolveLocallyAndFetchAbsentOrigins()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString absent = "2b2b2b2b-2b2b-4b2b-8b2b-2b2b2b2b2b2b";
        const QString gone = "3c3c3c3c-3c3c-4c3c-8c3c-3c3c3c3c3c3c";
        const QString farId = "4d4d4d4d-4d4d-4d4d-8d4d-4d4d4d4d4d4d";
        auto origin = message("Origin text", messageId);
        origin["is_own"] = false; origin["author_uuid"] = peerId;
        auto replied = message(QString("[Bobby](urn:quote:%1)\n\nanswer here").arg(messageId), returnedId);
        auto quotedRemote = message(QString("[Far](urn:quote:%1)\n\nremote answer").arg(absent), farId);
        auto quotedGone = message(QString("[Gone](urn:quote:%1?text=kept%20selection)\n\nlate").arg(gone),
                                  "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e");
        int absentLookups = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) {
                if (query.queryItemValue("uuid") == absent) {
                    ++absentLookups;
                    return Response{QJsonArray{message("Far body", absent, secondTopicId)}};
                }
                return Response{QJsonArray{}};
            }
            return Response{QJsonArray{origin, replied, quotedRemote, quotedGone}};
        };
        QVERIFY(fixture.start());

        // Origin inside the loaded window resolves synchronously.
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("quoteId").toString(), messageId);
        QCOMPARE(row.value("quoteLabel").toString(), QString("Bobby"));
        QVERIFY(row.value("quoteText").toString().isEmpty());
        QCOMPARE(row.value("quoteStatus").toString(), QString("ready"));
        QCOMPARE(row.value("quoteAuthorName").toString(), QString("Bobby"));
        QCOMPARE(row.value("quoteContent").toString(), QString("Origin text"));
        QCOMPARE(row.value("displayContent").toString(), QString("answer here"));

        // An origin outside the window fetches GET /messages/?uuid= exactly once.
        auto remote = modelMessage(fixture.controller, farId);
        QCOMPARE(remote.value("quoteStatus").toString(), QString("loading"));
        QTRY_COMPARE(modelMessage(fixture.controller, farId).value("quoteStatus").toString(), QString("ready"));
        QCOMPARE(modelMessage(fixture.controller, farId).value("quoteContent").toString(), QString("Far body"));
        QCOMPARE(absentLookups, 1);

        // A missing origin becomes the reference's "Message unavailable" state;
        // the ?text= selection survives decoding.
        QTRY_COMPARE(modelMessage(fixture.controller, "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e")
            .value("quoteStatus").toString(), QString("unavailable"));
        QCOMPARE(modelMessage(fixture.controller, "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e")
            .value("quoteText").toString(), QString("kept selection"));
    }

    // Issue #222: a fetched encrypted origin is not an authoritative miss. The
    // lookup stores it as a decrypt-pending record and the row renders the
    // locked "Key required" quote state instead of "Message unavailable".
    void quoteLookupOfEncryptedOriginRendersTheLockedState()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString encOrigin = "6f6f6f6f-6f6f-4f6f-8f6f-6f6f6f6f6f6f";
        auto quoted = message(QString("[Ema](urn:quote:%1)\n\nanswer").arg(encOrigin), returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) {
                if (query.queryItemValue("uuid") == encOrigin)
                    return Response{QJsonArray{QJsonObject{
                        {"uuid", encOrigin}, {"project_id", projectId}, {"stream_uuid", streamId},
                        {"topic_uuid", secondTopicId}, {"author_uuid", "7f7f7f7f-7f7f-4f7f-8f7f-7f7f7f7f7f7f"},
                        {"is_own", false}, {"read", true}, {"starred", false},
                        {"created_at", "2026-01-02T10:00:00.000Z"},
                        {"payload", QJsonObject{{"kind", "e2ee"},
                            {"key_uuid", "8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e"},
                            {"content", "ZmFrZS1lbnZlbG9wZQ=="}}}}}};
                return Response{QJsonArray{}};
            }
            return Response{QJsonArray{quoted}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("quoteStatus").toString(), QString("loading"));
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("quoteStatus").toString(), QString("locked"));
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("quoteId").toString(), encOrigin);
        QCOMPARE(row.value("quoteAuthorName").toString(), QString("Ema"));
        QVERIFY(row.value("quoteContent").toString().isEmpty());
        QCOMPARE(row.value("displayContent").toString(), QString("answer"));
    }

    void fetchedEncryptedQuoteDoesNotResurfaceAfterConfirmedMutation_data()
    {
        QTest::addColumn<bool>("removeOrigin");
        QTest::newRow("confirmed-edit") << false;
        QTest::newRow("confirmed-delete") << true;
    }

    void fetchedEncryptedQuoteDoesNotResurfaceAfterConfirmedMutation()
    {
        QFETCH(bool, removeOrigin);
        Fixture fixture;
        fixture.extraTopic = true;
        fixture.aggregateMessages = true;
        LoopbackKeyTls tls(fixture.server.serverPort());
        QTemporaryDir directory;
        ControllerMemorySecrets secrets;
        NativeChatKeyVault vault(directory.path(), &secrets);
        auto key = NativeChatCrypto::createGroupKey(); QVERIFY(key);
        const auto wipe = qScopeGuard([&] { key.bytes.fill(0); key.bytes.clear(); });
        const QString originId = "6f6f6f6f-6f6f-4f6f-8f6f-6f6f6f6f6f6f";
        const QString keyId = "8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e";
        const auto sealed = NativeChatCrypto::encryptMessage(key.bytes,
            "Pre-edit encrypted origin", {streamId, keyId});
        QVERIFY(sealed);
        auto origin = message({}, originId, secondTopicId);
        origin["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", keyId},
            {"content", QString::fromLatin1(sealed.bytes.toBase64())}};
        const auto reply = message(QStringLiteral("[Author](urn:quote:%1)\n\nanswer").arg(originId), returnedId);
        bool deleted = false;
        int lookups = 0, puts = 0, deletes = 0, confirmations = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto query = QUrlQuery(request.target);
            if (request.method == "PUT") {
                ++puts; origin["payload"] = request.body.value("payload");
                return {QJsonObject{}};
            }
            if (request.method == "DELETE") {
                ++deletes; deleted = true; return {QJsonObject{}};
            }
            if (path.endsWith("/" + originId)) {
                ++confirmations; return {origin};
            }
            if (query.hasQueryItem("uuid")) {
                ++lookups; return {deleted ? QJsonArray{} : QJsonArray{origin}};
            }
            if (query.queryItemValue("topic_uuid") == secondTopicId)
                return {deleted ? QJsonArray{} : QJsonArray{origin}};
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start(tls.origin()));
        bool opened = false;
        vault.open(tls.origin(), projectId, userId, "Synthetic quote device",
            [&](NativeChatKeyVault::Failure failure) { opened = failure == NativeChatKeyVault::Failure::None; });
        QTRY_VERIFY(opened);
        QCOMPARE(vault.put(vault.lease(), {streamId, keyId}, key.bytes, 0), NativeChatKeyVault::Failure::None);
        fixture.controller.useChatKeys(&vault);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("quoteStatus").toString(), QString("ready"));
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("quoteContent").toString(),
            QString("Pre-edit encrypted origin"));
        QCOMPARE(lookups, 1);
        QVERIFY(modelMessage(fixture.controller, originId).isEmpty());

        // The API permits payload-kind replacement in ordinary streams. Another
        // client has replaced the encrypted payload before Native loads it;
        // Native's own editor only accepts this now-markdown revision.
        if (!removeOrigin) origin["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Latest origin"}};
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy() && !modelMessage(fixture.controller, originId).isEmpty());
        if (removeOrigin) {
            fixture.controller.deleteMessage(originId);
            QTRY_COMPARE(deletes, 1);
            QTRY_VERIFY(modelMessage(fixture.controller, originId).isEmpty());
        } else {
            QVERIFY(fixture.controller.beginEditMessage(originId));
            fixture.controller.setDraft("Confirmed edited origin");
            fixture.controller.submitEditMessage();
            QTRY_COMPARE(puts, 1);
            QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
            QCOMPARE(confirmations, 1);
            QCOMPARE(modelMessage(fixture.controller, originId).value("content").toString(),
                QString("Confirmed edited origin"));
        }

        // Evict the source through real conversation navigation. Only the
        // retained quote record can answer until a fresh UUID lookup returns.
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(!fixture.controller.busy() && !modelMessage(fixture.controller, returnedId).isEmpty());
        QVERIFY(modelMessage(fixture.controller, originId).isEmpty());
        QVERIFY2(modelMessage(fixture.controller, returnedId).value("quoteContent").toString()
            != "Pre-edit encrypted origin", "confirmed mutation resurrected cached encrypted plaintext");
        QTRY_COMPARE(lookups, 2);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("quoteStatus").toString(),
            removeOrigin ? QString("unavailable") : QString("ready"));
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("quoteContent").toString(),
            removeOrigin ? QString{} : QString("Confirmed edited origin"));
    }

    void openMessageInChatAnchorsLoadedMessagesAndReportsUnknownOnes()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        auto note = message("Peer note", messageId);
        note["is_own"] = false;
        fixture.messages = [&](const Request &) -> Response { return Response{QJsonArray{note}}; };
        QVERIFY(fixture.start());
        fixture.controller.openMessageInChat(messageId);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), messageId);
        fixture.controller.openMessageInChat("6f6f6f6f-6f6f-4f6f-8f6f-6f6f6f6f6f6f");
        QTRY_VERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
    }
};

QTEST_MAIN(ControllerRepliesTest)
#include "native-controller-replies-test.moc"
