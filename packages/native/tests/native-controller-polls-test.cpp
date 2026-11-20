#include "workspace-reference.h"
#include "support/native-test-harness.h"
#include "support/controller-chat-creation-fixtures.h"
#include "support/controller-poll-fixtures.h"

class ControllerPollsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Issue #214 polls foundation ----

    void pollSnapshotCountsInQuotedMessages_data()
    {
        QTest::addColumn<QString>("visibility");
        QTest::addColumn<bool>("closed");
        QTest::addColumn<bool>("voted");
        QTest::addColumn<QString>("deadline");
        QTest::addColumn<bool>("fetched");
        QTest::addColumn<QString>("expected");
        const QString hidden = "Which day?\n• Monday\n• Tuesday\n\nWhere?\n• Office\n• Online";
        const QString first = "Which day?\n• Monday (1)\n• Tuesday (2)\n\nWhere?\n• Office\n• Online";
        const QString all = "Which day?\n• Monday (1)\n• Tuesday (2)\n\nWhere?\n• Office (3)\n• Online (0)";
        QTest::newRow("hidden") << QString("after_vote") << false << false << QString() << false << hidden;
        QTest::newRow("one-question-voted") << QString("after_vote") << false << true
            << QString("2099-01-01T00:00:00Z") << true << first;
        QTest::newRow("always") << QString("always") << false << false << QString() << false << all;
        QTest::newRow("closed") << QString("after_vote") << true << false << QString() << true << all;
        QTest::newRow("expired") << QString("after_vote") << false << false
            << QString("2000-01-01T00:00:00Z") << false << all;
    }

    void pollSnapshotCountsInQuotedMessages()
    {
        QFETCH(QString, visibility); QFETCH(bool, closed); QFETCH(bool, voted);
        QFETCH(QString, deadline); QFETCH(bool, fetched); QFETCH(QString, expected);
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto id = pollFixtureUuid(54);
        auto poll = pollMessageDto(id, 2, closed, "Which day?", voted);
        auto payload = poll.value("payload").toObject();
        payload["results_visibility"] = visibility;
        payload["closes_at"] = deadline.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(deadline);
        poll["payload"] = payload;
        int pollLookups = 0;
        const auto reply = message(buildWorkspaceQuoteReference("Peer", id) + "\n\nreply", returnedId);
        fixture.messages = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).queryItemValue("uuid") == id) {
                ++pollLookups;
                return {QJsonArray{poll}};
            }
            return {fetched ? QJsonArray{reply} : QJsonArray{poll, reply}};
        };
        QVERIFY(fixture.start());
        QTRY_COMPARE_WITH_TIMEOUT(modelMessage(fixture.controller, returnedId).value("quoteContent").toString(),
            expected, 2000);
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("quoteStatus").toString(), QString("ready"));
        QCOMPARE(pollLookups, fetched ? 1 : 0);
    }

    void pollSnapshotSkipsMissingQuestionStateAndCountsMissingOptionsAsZero()
    {
        auto poll = parseMessage(pollMessageDto(pollFixtureUuid(56), 3, true), projectId);
        QVERIFY(poll && poll->pollState);
        poll->pollState->questions.removeFirst();
        poll->pollState->questions.first().optionCounts.remove(pollOptionThree);
        QCOMPARE(pollTextSnapshot(*poll), QString("Which day?\n• Monday\n• Tuesday\n\nWhere?\n• Office (0)\n• Online (0)"));
    }

    void pollComposerEmbedsPlainSummaryAndForwardStoresIt()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto id = pollFixtureUuid(55);
        auto poll = pollMessageDto(id, 5, false, "**Day?**");
        auto payload = poll.value("payload").toObject();
        payload["closes_at"] = QJsonValue::Null;
        poll["payload"] = payload;
        const QString summary = "**Day?**\n• Monday\n• Tuesday\n\nWhere?\n• Office\n• Online";
        QJsonObject posted;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") {
                posted = request.body;
                return {message(posted["payload"].toObject()["content"].toString(), returnedId), 201};
            }
            if (request.target.path().endsWith("/messages/")) return {QJsonArray{poll}};
            return {message(posted["payload"].toObject()["content"].toString(), returnedId)};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.beginReply(id, "Monday"));
        QCOMPARE(fixture.controller.replyExcerpt(), summary.simplified());
        fixture.controller.setDraft("Answer");
        fixture.controller.send();
        QTRY_VERIFY(!posted.isEmpty());
        const auto quoteWire = posted["payload"].toObject()["content"].toString();
        QVERIFY(quoteWire.contains(buildWorkspaceQuoteUrn(id, summary)));
        QTRY_VERIFY(fixture.controller.replyMessageId().isEmpty());
        posted = {};
        fixture.controller.forwardMessages({id}, streamId, topicId);
        QTRY_VERIFY_WITH_TIMEOUT(!posted.isEmpty(), 2000);
        const auto wire = posted["payload"].toObject()["content"].toString();
        const auto start = wire.indexOf("(urn:forward:");
        QVERIFY(start >= 0 && wire.endsWith(')'));
        const auto ref = parseWorkspaceReference(wire.mid(start + 1, wire.size() - start - 2));
        QVERIFY(ref && ref->plainSnapshot);
        QCOMPARE(ref->snapshotMarkdown, summary);
    }

    void pollManagementUsesVerifiedAuthorAndRejectsPeerActions_data()
    {
        QTest::addColumn<bool>("author"); QTest::addColumn<bool>("ownFlag");
        QTest::newRow("author-without-convenience-flag") << true << false;
        QTest::newRow("peer-with-inconsistent-flag") << false << true;
    }

    void pollManagementUsesVerifiedAuthorAndRejectsPeerActions()
    {
        QFETCH(bool, author); QFETCH(bool, ownFlag);
        Fixture fixture;
        const auto id = pollFixtureUuid(81);
        auto poll = pollMessageDto(id, 3, false);
        poll["author_uuid"] = author ? userId : pollFixtureUuid(82);
        poll["is_own"] = ownFlag;
        auto payload = poll["payload"].toObject(); payload["closes_at"] = QJsonValue::Null; poll["payload"] = payload;
        int posts = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++posts; return {QJsonObject{}, 403}; }
            return {QJsonArray{poll}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.canManagePoll(id), author);
        QSignalSpy failure(&fixture.controller, &NativeController::pollActionFailed);
        fixture.controller.closePoll(id);
        QTRY_COMPARE(failure.size(), 1);
        QCOMPARE(posts, author ? 1 : 0);
        QCOMPARE(failure.first().at(2).toString(), author ? QString("http-403") : QString("not-available"));
        QVERIFY(!modelMessage(fixture.controller, id).value("poll").toMap().value("state").toMap().value("isClosed").toBool());
    }

    void pollMessageArrivesEndToEndWithEveryConfigField()
    {
        Fixture fixture;
        const auto id = pollFixtureUuid(1);
        fixture.messages = [id](const Request &) {
            return Response{QJsonArray{pollMessageDto(id, 42, false,
                QStringLiteral("Which day?"), true)}, 200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        const auto poll = modelMessage(fixture.controller, id).value("poll").toMap();
        QVERIFY(!poll.isEmpty());
        QCOMPARE(poll.value("multipleAnswers").toBool(), true);
        QCOMPARE(poll.value("isAnonymous").toBool(), false);
        QCOMPARE(poll.value("allowRevote").toBool(), false);
        QCOMPARE(poll.value("resultsVisibility").toString(), QStringLiteral("after_vote"));
        QCOMPARE(poll.value("allowAddingOptions").toBool(), true);
        QCOMPARE(poll.value("closesAt").toString(), QStringLiteral("2026-12-12T09:00:00Z"));
        QCOMPARE(poll.value("closedAt").toString(), QString());
        const auto questions = poll.value("questions").toList();
        QCOMPARE(questions.size(), 2);
        QCOMPARE(questions.first().toMap().value("text").toString(), QStringLiteral("Which day?"));
        QCOMPARE(questions.first().toMap().value("options").toList().size(), 2);
        const auto state = poll.value("state").toMap();
        QCOMPARE(state.value("revision").toLongLong(), qint64(42));
        QCOMPARE(state.value("isClosed").toBool(), false);
        QCOMPARE(state.value("voterCount").toLongLong(), qint64(3));
        QCOMPARE(state.value("myVotes").toList().size(), 1);
        QCOMPARE(state.value("questions").toList().first().toMap()
            .value("optionCounts").toMap().value(pollOptionTwo).toLongLong(), qint64(2));
    }

    void malformedPollPageIsRejectedAsAWhole()
    {
        Fixture fixture;
        const auto good = pollFixtureUuid(11);
        const auto broken = pollFixtureUuid(12);
        auto brokenRow = pollMessageDto(broken, 7, false);
        brokenRow.remove(QStringLiteral("poll_state")); // poll requires poll_state
        fixture.messages = [&](const Request &) {
            return Response{QJsonArray{message(QStringLiteral("committed"), good), brokenRow},
                200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.busy(), 3000);
        QTest::qWait(300);
        // One invalid row rejects the whole page: not even its valid rows are
        // applied partially, so the malformed poll can never slip in alongside
        // the good row.
        QVERIFY(modelMessage(fixture.controller, good).isEmpty());
        QVERIFY(modelMessage(fixture.controller, broken).isEmpty());
    }

    void pollRealtimeNeverRollsBackNewerRevisionOrClosedFlag()
    {
        Fixture fixture;
        const auto id = pollFixtureUuid(2);
        fixture.messages = [id](const Request &) {
            return Response{QJsonArray{pollMessageDto(id, 5, false, QStringLiteral("Fresh question"))},
                200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated",
            pollMessageDto(id, 4, true, QStringLiteral("Stale question")), 7701)));
        QTRY_VERIFY_WITH_TIMEOUT(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("state").toMap().value("revision").toLongLong() == 5, 2000);
        {
            const auto poll = modelMessage(fixture.controller, id).value("poll").toMap();
            QCOMPARE(poll.value("questions").toList().first().toMap()
                .value("text").toString(), QStringLiteral("Fresh question"));
            QCOMPARE(poll.value("state").toMap().value("isClosed").toBool(), false);
        }
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated",
            pollMessageDto(id, 6, true, QStringLiteral("Fresh question")), 7702)));
        QTRY_VERIFY_WITH_TIMEOUT(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("state").toMap().value("isClosed").toBool(), 2000);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated",
            pollMessageDto(id, 6, false, QStringLiteral("Fresh question")), 7703)));
        QTest::qWait(250);
        QCOMPARE(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("state").toMap().value("isClosed").toBool(), true);
    }

    void votePollInvokePostsDesiredStateAndKeepsOrderAndReadState()
    {
        Fixture fixture;
        const auto id = pollFixtureUuid(3);
        const auto older = pollFixtureUuid(30);
        auto olderRow = message(QStringLiteral("older"), older);
        olderRow.insert(QStringLiteral("created_at"), QStringLiteral("2026-01-02T10:00:00.000Z"));
        QJsonObject capturedVoteBody;
        int votePosts = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST" && request.target.path().endsWith("/actions/vote/invoke")) {
                ++votePosts;
                capturedVoteBody = request.body;
                const auto selected = request.body.value(QStringLiteral("selected")).toBool();
                return Response{pollMessageDto(id, 6, false, QStringLiteral("Which day?"), selected),
                    200, 0, {}};
            }
            return Response{QJsonArray{olderRow, pollMessageDto(id, 5, false)}, 200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        QStringList before;
        for (const auto &row : fixture.controller.messages())
            before << row.toMap().value("id").toString();
        QSignalSpy failed(&fixture.controller, &NativeController::pollActionFailed);
        // desired-state semantics: an un-vote intent is posted verbatim, not toggled.
        fixture.controller.votePollOption(id, pollQuestionOne, pollOptionOne, false);
        QTRY_VERIFY_WITH_TIMEOUT(votePosts == 1, 2000);
        QCOMPARE(capturedVoteBody.value(QStringLiteral("question_uuid")).toString(), pollQuestionOne);
        QCOMPARE(capturedVoteBody.value(QStringLiteral("option_uuid")).toString(), pollOptionOne);
        QCOMPARE(capturedVoteBody.value(QStringLiteral("selected")).toBool(), false);
        fixture.controller.votePollOption(id, pollQuestionOne, pollOptionOne, true);
        QTRY_VERIFY_WITH_TIMEOUT(votePosts == 2, 2000);
        QCOMPARE(capturedVoteBody.value(QStringLiteral("selected")).toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(pollStateHasMyVote(modelMessage(fixture.controller, id)
            .value("poll").toMap().value("state").toMap(), pollOptionOne), 2000);
        QCOMPARE(failed.count(), 0);
        QStringList after;
        for (const auto &row : fixture.controller.messages())
            after << row.toMap().value("id").toString();
        QCOMPARE(after, before); // voting never reorders the window
        QCOMPARE(modelMessage(fixture.controller, id).value("read").toBool(), true);
    }

    void addPollOptionReplayOfSameUuidAndTextIsAccepted()
    {
        Fixture fixture;
        const auto id = pollFixtureUuid(5);
        const auto optionUuid = pollFixtureUuid(95);
        int adds = 0;
        QJsonObject capturedBody;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST"
                && request.target.path().endsWith("/actions/add_poll_option/invoke")) {
                ++adds;
                capturedBody = request.body;
                // Contract: a uuid+text replay is 200 with the unchanged message,
                // no revision bump and no event - even on a closed poll.
                return Response{pollMessageDto(id, 4, true), 200, 0, {}};
            }
            return Response{QJsonArray{pollMessageDto(id, 4, true)}, 200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        QSignalSpy failed(&fixture.controller, &NativeController::pollActionFailed);
        fixture.controller.addPollOption(id, pollQuestionOne, optionUuid, QStringLiteral("Wednesday"));
        QTRY_VERIFY_WITH_TIMEOUT(adds == 1, 2000);
        QCOMPARE(capturedBody.value(QStringLiteral("question_uuid")).toString(), pollQuestionOne);
        QCOMPARE(capturedBody.value(QStringLiteral("option")).toObject()
            .value(QStringLiteral("uuid")).toString(), optionUuid);
        QCOMPARE(capturedBody.value(QStringLiteral("option")).toObject()
            .value(QStringLiteral("text")).toString(), QStringLiteral("Wednesday"));
        fixture.controller.addPollOption(id, pollQuestionOne, optionUuid, QStringLiteral("Wednesday"));
        QTRY_VERIFY_WITH_TIMEOUT(adds == 2, 2000);
        QTest::qWait(250);
        QCOMPARE(failed.count(), 0);
        const auto options = modelMessage(fixture.controller, id).value("poll").toMap()
            .value("questions").toList().first().toMap().value("options").toList();
        int duplicates = 0;
        for (const auto &option : options)
            if (option.toMap().value("uuid").toString() == optionUuid) ++duplicates;
        QCOMPARE(duplicates, 0); // the unchanged response cannot invent options
        QCOMPARE(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("state").toMap().value("revision").toLongLong(), qint64(4));
    }

    void closePollInvokeSendsEmptyBodyAndMarksClosed()
    {
        Fixture fixture;
        const auto id = pollFixtureUuid(6);
        int closes = 0;
        bool closeBodyEmpty = false;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST" && request.target.path().endsWith("/actions/close_poll/invoke")) {
                ++closes;
                closeBodyEmpty = request.body.isEmpty();
                auto dto = pollMessageDto(id, 7, true);
                auto payload = dto.value(QStringLiteral("payload")).toObject();
                payload.insert(QStringLiteral("closed_at"), QStringLiteral("2026-01-02T10:05:00Z"));
                dto.insert(QStringLiteral("payload"), payload);
                return Response{dto, 200, 0, {}};
            }
            return Response{QJsonArray{pollMessageDto(id, 5, false)}, 200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        QSignalSpy failed(&fixture.controller, &NativeController::pollActionFailed);
        fixture.controller.closePoll(id);
        QTRY_VERIFY_WITH_TIMEOUT(closes == 1, 2000);
        QVERIFY(closeBodyEmpty);
        QTRY_VERIFY_WITH_TIMEOUT(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("state").toMap().value("isClosed").toBool(), 2000);
        QCOMPARE(failed.count(), 0);
    }

    void updatePollSettingsSendsBothFieldsAsFullConfig()
    {
        Fixture fixture;
        const auto id = pollFixtureUuid(7);
        QJsonObject capturedBody;
        int settings = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST"
                && request.target.path().endsWith("/actions/update_settings/invoke")) {
                ++settings;
                capturedBody = request.body;
                auto dto = pollMessageDto(id, 8, false);
                auto payload = dto.value(QStringLiteral("payload")).toObject();
                payload.insert(QStringLiteral("allow_adding_options"), false);
                payload.insert(QStringLiteral("closes_at"), QJsonValue::Null);
                dto.insert(QStringLiteral("payload"), payload);
                return Response{dto, 200, 0, {}};
            }
            return Response{QJsonArray{pollMessageDto(id, 5, false)}, 200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        QSignalSpy failed(&fixture.controller, &NativeController::pollActionFailed);
        fixture.controller.updatePollSettings(id, QString(), false);
        QTRY_VERIFY_WITH_TIMEOUT(settings == 1, 2000);
        const auto settingsObject = capturedBody.value(QStringLiteral("settings")).toObject();
        QVERIFY(settingsObject.contains(QStringLiteral("closes_at")));
        QVERIFY(settingsObject.value(QStringLiteral("closes_at")).isNull());
        QVERIFY(settingsObject.contains(QStringLiteral("allow_adding_options")));
        QCOMPARE(settingsObject.value(QStringLiteral("allow_adding_options")).toBool(), false);
        QTRY_VERIFY_WITH_TIMEOUT(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("allowAddingOptions").toBool() == false, 2000);
        QCOMPARE(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("closesAt").toString(), QString());
        QCOMPARE(failed.count(), 0);
    }

    void sendPollRequiresEligibleStreamAndSendsClientGeneratedUuids()
    {
        Fixture fixture;
        const auto created = pollFixtureUuid(8);
        QJsonObject createdBody;
        int creates = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST" && request.target.path().endsWith("/messages/")) {
                ++creates;
                createdBody = request.body;
                return Response{pollMessageDto(created, 1, false), 200, 0, {}};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.canCreatePoll());
        const QVariantList questions{
            QVariantMap{{QString("text"), QStringLiteral("Which day?")}, {QString("options"), QVariantList{
                QVariantMap{{QString("text"), QStringLiteral("Monday")}},
                QVariantMap{{QString("text"), QStringLiteral("Tuesday")}}}}}};
        fixture.controller.sendPoll(questions, true, false, false,
            QStringLiteral("after_vote"), false, QStringLiteral("2026-12-12T09:00:00Z"));
        QTRY_VERIFY_WITH_TIMEOUT(creates == 1, 2000);
        const auto payload = createdBody.value(QStringLiteral("payload")).toObject();
        QCOMPARE(payload.value(QStringLiteral("kind")).toString(), QStringLiteral("poll"));
        const auto questionsArray = payload.value(QStringLiteral("questions")).toArray();
        QCOMPARE(questionsArray.size(), 1);
        const auto question = questionsArray.at(0).toObject();
        QVERIFY(isUuid(question.value(QStringLiteral("uuid")).toString()));
        const auto options = question.value(QStringLiteral("options")).toArray();
        QCOMPARE(options.size(), 2);
        for (const auto &option : options)
            QVERIFY(isUuid(option.toObject().value(QStringLiteral("uuid")).toString()));
        QCOMPARE(payload.value(QStringLiteral("multiple_answers")).toBool(), true);
        QCOMPARE(payload.value(QStringLiteral("results_visibility")).toString(),
                 QStringLiteral("after_vote"));
        QCOMPARE(payload.value(QStringLiteral("closes_at")).toString(),
                 QStringLiteral("2026-12-12T09:00:00Z"));
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, created).isEmpty(), 2000);
        // Encrypted streams are never poll-eligible (supportsMessengerPolls).
        Fixture encrypted;
        encrypted.encrypted = true;
        QVERIFY(encrypted.start());
        QVERIFY(!encrypted.controller.canCreatePoll());
        encrypted.controller.sendPoll(questions, false, false, false,
            QStringLiteral("always"), false, QString());
        QTest::qWait(300);
        for (const auto &request : encrypted.server.requests)
            QVERIFY(!(request.method == "POST" && request.target.path().endsWith("/messages/")));
    }

    void sendPollCompletionStaysWithItsConversation_data()
    {
        QTest::addColumn<QString>("navigation");
        QTest::addColumn<bool>("realtimeFirst");
        for (const auto *navigation : {"stay", "other-topic", "other-stream", "stream", "return"})
            for (const bool realtimeFirst : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(navigation,
                    realtimeFirst ? "realtime-first" : "response-only")))
                    << QString::fromLatin1(navigation) << realtimeFirst;
    }

    void sendPollCompletionStaysWithItsConversation()
    {
        QFETCH(QString, navigation);
        QFETCH(bool, realtimeFirst);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture;
        fixture.aggregateMessages = true;
        auto &controller = fixture.controller;
        controller.useCache(&cache);
        const auto source = message("Source message");
        const auto destination = message("Other topic message", returnedId, secondTopicId);
        const auto otherStream = pollFixtureUuid(98);
        const auto otherTopic = pollFixtureUuid(97);
        auto otherStreamMessage = destination;
        otherStreamMessage.insert("stream_uuid", otherStream);
        otherStreamMessage.insert("topic_uuid", otherTopic);
        QJsonObject created;
        QVector<QJsonObject> creates;
        std::function<void()> release;
        const auto releaseOnExit = qScopeGuard([&] { if (release) release(); });
        fixture.topics = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == otherStream)
                return Response{QJsonArray{creationTopic(otherTopic, otherStream, "Other stream topic")}};
            return Response{QJsonArray{creationTopic(topicId, streamId, "Source topic"),
                creationTopic(secondTopicId, streamId, "Other topic")}};
        };
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                creates.append(request.body);
                created = pollMessageDto(request.body.value("uuid").toString(), 1, false);
                Response response{created};
                if (creates.size() == 1)
                    response.defer = [&](std::function<void()> send) { release = std::move(send); };
                return response;
            }
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == otherStream)
                return Response{QJsonArray{otherStreamMessage}};
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{topic == secondTopicId ? QJsonArray{destination} : QJsonArray{source}};
        };
        QVERIFY(fixture.start());
        if (navigation == "other-stream")
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.created",
                QJsonObject{{"uuid", otherStream}, {"project_id", projectId},
                    {"name", "Other stream"}, {"encryption", false}}, 7900)));
        QTRY_VERIFY(cache.available());
        QVERIFY(flushCache(cache));
        const QVariantList questions{
            QVariantMap{{QString("text"), QStringLiteral("Which day?")}, {QString("options"), QVariantList{
                QVariantMap{{QString("text"), QStringLiteral("Monday")}},
                QVariantMap{{QString("text"), QStringLiteral("Tuesday")}}}}}};
        QSignalSpy completed(&controller, &NativeController::pollCreated);
        QSignalSpy failed(&controller, &NativeController::pollActionFailed);
        controller.sendPoll(questions, false, false, false, QStringLiteral("always"), false, {});
        QTRY_VERIFY(bool(release));
        QCOMPARE(creates.size(), 1);
        const auto id = created.value("uuid").toString();
        if (realtimeFirst) {
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("message.created", created, 7901)));
            QTRY_VERIFY(!modelMessage(controller, id).isEmpty());
        }
        if (navigation == "other-topic" || navigation == "return") {
            controller.selectTopic(secondTopicId);
            QTRY_VERIFY(!controller.busy());
            QTRY_VERIFY(!modelMessage(controller, returnedId).isEmpty());
        } else if (navigation == "other-stream") {
            controller.selectStream(otherStream);
            QTRY_VERIFY(!controller.busy());
            QTRY_VERIFY(!modelMessage(controller, returnedId).isEmpty());
        } else if (navigation == "stream") {
            controller.selectTopic({});
            QTRY_VERIFY(!controller.busy());
            QTRY_VERIFY(!modelMessage(controller, messageId).isEmpty());
        }
        if (navigation == "return") {
            controller.selectTopic(topicId);
            QTRY_VERIFY(!controller.busy());
            QTRY_VERIFY(!modelMessage(controller, messageId).isEmpty());
        }
        QVERIFY(flushCache(cache));
        const auto before = controller.messages();
        QSignalSpy changed(&controller, &NativeController::messagesChanged);
        auto send = std::move(release); release = {}; send();
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(completed.first(), QVariantList({id, streamId, topicId}));
        QCOMPARE(failed.count(), 0);
        if (navigation == "other-topic" || navigation == "other-stream") {
            QCOMPARE(controller.messages(), before);
            QCOMPARE(changed.count(), 0);
            QVERIFY(modelMessage(controller, id).isEmpty());
        } else {
            int occurrences = 0;
            for (const auto &row : controller.messages())
                if (row.toMap().value("id").toString() == id) ++occurrences;
            QCOMPARE(occurrences, 1);
        }
        // The accepted send still reaches its original cached page after navigation.
        const auto saved = cacheEntry(cache, cache.lease(),
            {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(saved);
        int cachedOccurrences = 0;
        for (const auto &row : saved->rows)
            if (row.toObject().value("uuid").toString() == id) ++cachedOccurrences;
        QCOMPARE(cachedOccurrences, 1);
        if (navigation == "other-topic") {
            const auto other = cacheEntry(cache, cache.lease(),
                {NativeCache::Kind::History, streamId, secondTopicId, {}});
            QVERIFY(other);
            for (const auto &row : other->rows)
                QVERIFY(row.toObject().value("uuid").toString() != id);
        }
        // Completion retires A's attempt even while another conversation is open.
        if (controller.selectedStream() != streamId) {
            controller.selectStream(streamId);
            QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(controller.topics().size(), 2);
        }
        controller.selectTopic(topicId);
        QTRY_VERIFY(!controller.busy());
        controller.sendPoll(questions, false, false, false, QStringLiteral("always"), false, {});
        QTRY_COMPARE(completed.count(), 2);
        QCOMPARE(creates.size(), 2);
        QVERIFY(creates.last().value("uuid").toString() != id);
    }

    void sendPollLateResponseCannotCrossSessionGeneration_data()
    {
        QTest::addColumn<bool>("logout");
        QTest::newRow("token-replacement") << false;
        QTest::newRow("sign-out") << true;
    }

    void sendPollLateResponseCannotCrossSessionGeneration()
    {
        QFETCH(bool, logout);
        Fixture fixture;
        std::function<void()> release;
        const auto releaseOnExit = qScopeGuard([&] { if (release) release(); });
        fixture.messages = [&](const Request &request) {
            if (request.method != "POST") return Response{QJsonArray{message()}};
            Response response{pollMessageDto(request.body.value("uuid").toString(), 1, false)};
            response.defer = [&](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        const QVariantList questions{
            QVariantMap{{QString("text"), QStringLiteral("Which day?")}, {QString("options"), QVariantList{
                QVariantMap{{QString("text"), QStringLiteral("Monday")}},
                QVariantMap{{QString("text"), QStringLiteral("Tuesday")}}}}}};
        QSignalSpy completed(&controller, &NativeController::pollCreated);
        controller.sendPoll(questions, false, false, false, QStringLiteral("always"), false, {});
        QTRY_VERIFY(bool(release));
        if (logout) controller.logout();
        else fixture.api.setAccessToken("synthetic-replaced-poll-token");
        const auto before = controller.messages();
        auto send = std::move(release); release = {}; send();
        QTest::qWait(100);
        QCOMPARE(controller.messages(), before);
        QCOMPARE(completed.count(), 0);
    }

    void sendPollRetryIdentityDoesNotSurviveSignOut()
    {
        Fixture fixture;
        QVector<QJsonObject> bodies;
        QVector<std::function<void()>> releases;
        fixture.messages = [&](const Request &request) {
            if (request.method != "POST") return Response{QJsonArray{}};
            bodies.append(request.body);
            Response response{pollMessageDto(request.body.value("uuid").toString(), 1, false)};
            response.defer = [&](std::function<void()> send) { releases.append(std::move(send)); };
            return response;
        };
        const QVariantList questions{
            QVariantMap{{QString("text"), QStringLiteral("Which day?")}, {QString("options"), QVariantList{
                QVariantMap{{QString("text"), QStringLiteral("Monday")}},
                QVariantMap{{QString("text"), QStringLiteral("Tuesday")}}}}}};
        QVERIFY(fixture.start());
        fixture.controller.sendPoll(questions, false, false, false, "always", false, {});
        QTRY_COMPARE(bodies.size(), 1);
        fixture.controller.logout();
        QVERIFY(fixture.start());
        fixture.controller.sendPoll(questions, false, false, false, "always", false, {});
        QTRY_COMPARE(bodies.size(), 2);
        QVERIFY(bodies[1].value("uuid") != bodies[0].value("uuid"));
        const auto first = bodies[0].value("payload").toObject().value("questions");
        const auto second = bodies[1].value("payload").toObject().value("questions");
        QVERIFY(second != first);
        for (auto &release : releases) release();
    }

    void sendPollIdenticalRetryReusesCanonicalUuidsUntilConfirmed()
    {
        Fixture fixture;
        const auto created = pollFixtureUuid(18);
        QVector<QJsonObject> bodies;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST" && request.target.path().endsWith("/messages/")) {
                bodies.append(request.body);
                // The first create is an uncertain 5xx; every later create confirms.
                if (bodies.size() == 1) return Response{QJsonObject{}, 503, 0, {}};
                return Response{pollMessageDto(created, 1, false), 200, 0, {}};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.canCreatePoll());
        QSignalSpy failed(&fixture.controller, &NativeController::pollActionFailed);
        const QVariantList questions{
            QVariantMap{{QString("text"), QStringLiteral("Which day?")}, {QString("options"), QVariantList{
                QVariantMap{{QString("text"), QStringLiteral("Monday")}, {QString("correct"), true}},
                QVariantMap{{QString("text"), QStringLiteral("Tuesday")}}}}}};
        const auto payloadOf = [](const QJsonObject &body) {
            return body.value(QStringLiteral("payload")).toObject();
        };
        const auto questionUuidOf = [&](const QJsonObject &body) {
            return payloadOf(body).value(QStringLiteral("questions")).toArray().at(0).toObject()
                .value(QStringLiteral("uuid")).toString();
        };
        const auto optionUuidsOf = [&](const QJsonObject &body) {
            QStringList uuids;
            const auto options = payloadOf(body).value(QStringLiteral("questions")).toArray()
                .at(0).toObject().value(QStringLiteral("options")).toArray();
            for (const auto &option : options)
                uuids << option.toObject().value(QStringLiteral("uuid")).toString();
            return uuids;
        };
        fixture.controller.sendPoll(questions, false, false, false,
            QStringLiteral("after_vote"), false, QString());
        QTRY_VERIFY_WITH_TIMEOUT(bodies.size() == 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("create"));
        QCOMPARE(failed.at(0).at(2).toString(), QStringLiteral("http-503"));
        // Create failures carry no question: the vote/add completion identity
        // (round 3 review) is empty for actions whose body has no question_uuid.
        QCOMPARE(failed.at(0).size(), 4);
        QCOMPARE(failed.at(0).at(3).toString(), QString());
        const auto messageUuid = bodies.first().value(QStringLiteral("uuid")).toString();
        QVERIFY(isUuid(messageUuid));
        const auto firstQuestionUuid = questionUuidOf(bodies.first());
        QVERIFY(isUuid(firstQuestionUuid));
        const auto firstOptionUuids = optionUuidsOf(bodies.first());
        QCOMPARE(firstOptionUuids.size(), 2);
        // The marked-correct answer travels as the client option uuid.
        const QStringList correct{payloadOf(bodies.first()).value(QStringLiteral("questions"))
            .toArray().at(0).toObject().value(QStringLiteral("correct_option_uuids")).toArray().at(0)
            .toString()};
        QCOMPARE(correct, QStringList{firstOptionUuids.first()});
        // An identical retry replays the same attempt: identical client UUIDs.
        fixture.controller.sendPoll(questions, false, false, false,
            QStringLiteral("after_vote"), false, QString());
        QTRY_VERIFY_WITH_TIMEOUT(bodies.size() == 2, 2000);
        QCOMPARE(bodies.at(1).value(QStringLiteral("uuid")).toString(), messageUuid);
        QCOMPARE(questionUuidOf(bodies.at(1)), firstQuestionUuid);
        QCOMPARE(optionUuidsOf(bodies.at(1)), firstOptionUuids);
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, created).isEmpty(), 2000);
        QCOMPARE(failed.count(), 1);
        // The confirmed attempt is retired: the next identical submit is a new one.
        fixture.controller.sendPoll(questions, false, false, false,
            QStringLiteral("after_vote"), false, QString());
        QTRY_VERIFY_WITH_TIMEOUT(bodies.size() == 3, 2000);
        QVERIFY(bodies.at(2).value(QStringLiteral("uuid")).toString() != messageUuid);
        QVERIFY(questionUuidOf(bodies.at(2)) != firstQuestionUuid);
        // Content changes always rotate the attempt (new canonical UUIDs).
        const QVariantList edited{
            QVariantMap{{QString("text"), QStringLiteral("Which day??")}, {QString("options"), QVariantList{
                QVariantMap{{QString("text"), QStringLiteral("Monday")}},
                QVariantMap{{QString("text"), QStringLiteral("Tuesday")}}}}}};
        fixture.controller.sendPoll(edited, false, false, false,
            QStringLiteral("after_vote"), false, QString());
        QTRY_VERIFY_WITH_TIMEOUT(bodies.size() == 4, 2000);
        QVERIFY(bodies.at(3).value(QStringLiteral("uuid")).toString()
            != bodies.at(2).value(QStringLiteral("uuid")).toString());
    }

    void pollVotersPageParsesStrictlyWithPositionalMarker()
    {
        Fixture fixture;
        const auto id = pollFixtureUuid(9);
        const auto voter = pollFixtureUuid(91);
        bool votersQueryOk = false;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/poll_voters/")) {
                const QUrlQuery query(request.target);
                votersQueryOk = query.queryItemValue(QStringLiteral("question_uuid")) == pollQuestionOne
                    && query.queryItemValue(QStringLiteral("option_uuid")) == pollOptionOne;
                if (query.queryItemValue(QStringLiteral("page_limit")) != QLatin1String("50")) {
                    const QJsonObject bad{{QStringLiteral("uuid"), QStringLiteral("nope")}};
                    return Response{QJsonArray{bad}, 200, 0, {}};
                }
                const QJsonObject row{{QStringLiteral("uuid"), voter}};
                const QMap<QByteArray, QByteArray> headers{
                    {QByteArray("X-Pagination-Marker"), QByteArray("next-marker")}};
                return Response{QJsonArray{row}, 200, 0, headers};
            }
            return Response{QJsonArray{pollMessageDto(id, 5, false)}, 200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        QSignalSpy loaded(&fixture.controller, &NativeController::pollVotersLoaded);
        fixture.controller.loadPollVoters(id, pollQuestionOne, pollOptionOne);
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 2000);
        QVERIFY(votersQueryOk);
        {
            const QList<QVariant> args = loaded.first();
            const QStringList expectedVoters{voter};
            QCOMPARE(args.at(0).toString(), id);
            QCOMPARE(args.at(3).toStringList(), expectedVoters);
            QCOMPARE(args.at(4).toString(), QStringLiteral("next-marker"));
            QCOMPARE(args.at(5).toString(), QString());
        }
        // A malformed item rejects the whole page (strict list parsing).
        fixture.controller.loadPollVoters(id, pollQuestionOne, pollOptionOne, QString(), 60);
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 2, 2000);
        {
            const QList<QVariant> args = loaded.at(1);
            QCOMPARE(args.at(3).toStringList(), QStringList{});
            QVERIFY(!args.at(5).toString().isEmpty());
        }
        // The client never asks above the server maximum.
        fixture.controller.loadPollVoters(id, pollQuestionOne, pollOptionOne, QString(), 500);
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 3, 2000);
        QVERIFY(!loaded.at(2).at(5).toString().isEmpty());
    }

    // Exordy threads 4563441897/4563441917 (duplicates): the voters callback
    // must not retain references to the caller's QString argument objects.
    // The response is held in flight, the heap-owned argument objects are
    // destroyed, their freed allocation slots are taken over by poison
    // strings, and only then the held response lands: a by-reference capture
    // would emit the poison strings (or worse) instead of the request arguments.
    void pollVotersDeferredResponseCarriesIntactArgumentsAfterCallerObjectsDie()
    {
        std::function<void()> releaseVoters;
        Fixture fixture;
        const auto id = pollFixtureUuid(17);
        const auto voter = pollFixtureUuid(171);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/poll_voters/")) {
                const QJsonObject row{{QStringLiteral("uuid"), voter}};
                Response response{QJsonArray{row}, 200, 0, {}};
                response.defer = [&releaseVoters](std::function<void()> send) { releaseVoters = std::move(send); };
                return response;
            }
            return Response{QJsonArray{pollMessageDto(id, 5, false)}, 200, 0, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        QSignalSpy loaded(&fixture.controller, &NativeController::pollVotersLoaded);
        QVERIFY(loaded.isValid());
        // Heap-owned argument objects: a callback that keeps a reference to
        // each caller object survives its destruction only by accident, so
        // destroying and re-poisoning the slots makes any dangling read
        // observable instead of stale-but-lucky.
        auto messageUuid = std::make_unique<QString>(id);
        auto questionUuid = std::make_unique<QString>(pollQuestionOne);
        auto optionUuid = std::make_unique<QString>(pollOptionOne);
        fixture.controller.loadPollVoters(*messageUuid, *questionUuid, *optionUuid);
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(releaseVoters), 2000);
        messageUuid.reset(); questionUuid.reset(); optionUuid.reset();
        // Heap poison: same-size allocations take over the freed argument
        // slots before the held response lands.
        auto poisonMessage = std::make_unique<QString>(pollFixtureUuid(777));
        auto poisonQuestion = std::make_unique<QString>(pollFixtureUuid(778));
        auto poisonOption = std::make_unique<QString>(pollFixtureUuid(779));
        releaseVoters(); releaseVoters = {};
        QVERIFY(loaded.wait(2000));
        const QList<QVariant> args = loaded.first();
        QCOMPARE(args.at(0).toString(), id);
        QCOMPARE(args.at(1).toString(), pollQuestionOne);
        QCOMPARE(args.at(2).toString(), pollOptionOne);
        const QStringList expectedVoters{voter};
        QCOMPARE(args.at(3).toStringList(), expectedVoters);
        // Keep the poison strings alive while the callback ran (no optimizer
        // escape: they were read through the dangling references if any).
        QVERIFY(poisonMessage->size() > 0 && poisonQuestion->size() > 0 && poisonOption->size() > 0);
    }

    // Exordy thread 4563441924: when the equal-revision reopen ratchet keeps
    // an already-closed poll closed in memory, the cache projection must
    // persist that guarded closed state, never the incoming reopened
    // poll_state. Here the cached head page no longer contains the poll row
    // (page rebuild/vacate shape), so the projection inserts the incoming row
    // verbatim - it must carry the guarded state, not the reopen.
    void pollEqualRevisionReopenCannotPersistOpenStateToCache()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), NativeCache::Policy{}, true);
        Fixture fixture;
        const auto id = pollFixtureUuid(21);
        const auto plainId = pollFixtureUuid(22);
        auto plain = message(QStringLiteral("Plain older message"), plainId);
        plain.insert(QStringLiteral("created_at"), QStringLiteral("2026-01-02T10:00:00.000Z"));
        fixture.messages = [&](const Request &) {
            return Response{QJsonArray{plain, pollMessageDto(id, 6, true)}, 200, 0, {}};
        };
        fixture.controller.useCache(&cache);
        QVERIFY(fixture.start());
        QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!modelMessage(fixture.controller, id).isEmpty(), 3000);
        // Rebuild the cached head page without the poll row: the next
        // projection of this message must insert it from its DTO.
        QVERIFY(cache.put(cache.lease(),
            {NativeCache::Kind::History, streamId, topicId, {}}, {plain}));
        // Equal-revision reopen attempt: the in-memory guard keeps the poll
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated",
            pollMessageDto(id, 6, false), 7801)));
        QTRY_VERIFY_WITH_TIMEOUT(modelMessage(fixture.controller, id).value("poll").toMap()
            .value("state").toMap().value("isClosed").toBool(), 2000);
        // closed; whatever row the projection persisted must agree with the
        // guarded state, so a later offline restore cannot reopen the poll.
        QJsonObject pollRow;
        const auto foundPoll = [&]() {
            const auto entry = cacheEntry(cache, cache.lease(),
                {NativeCache::Kind::History, streamId, topicId, {}});
            if (!entry) return false;
            for (const auto &row : entry->rows) {
                if (row.toObject().value(QStringLiteral("uuid")).toString() == id) {
                    pollRow = row.toObject();
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(foundPoll(), 3000);
        QCOMPARE(pollRow.value(QStringLiteral("poll_state")).toObject()
            .value(QStringLiteral("is_closed")).toBool(), true);
    }

    // The poll status date mirrors the reference locale rendering
    // (workspace-message-poll.ui.tsx:47-49): long month + HH:mm, in the app
    // locale that applyLanguage set as the QLocale default.
    void pollDateLabelFollowsTheAppLocale()
    {
        Fixture fixture; QVERIFY(fixture.start());
        const auto saved = QLocale();
        const auto restore = qScopeGuard([&saved] { QLocale::setDefault(saved); });
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QCOMPARE(fixture.controller.formatPollDate(QStringLiteral("2026-12-12T10:00:00")),
            QStringLiteral("12 December, 10:00"));
        QLocale::setDefault(QLocale(QLocale::Russian, QLocale::Russia));
        QCOMPARE(fixture.controller.formatPollDate(QStringLiteral("2026-12-12T10:00:00")),
            QStringLiteral("12 декабря, 10:00"));
        QCOMPARE(fixture.controller.formatPollDate(QStringLiteral("not-a-date")), QString{});
    }
};

QTEST_MAIN(ControllerPollsTest)
#include "native-controller-polls-test.moc"
