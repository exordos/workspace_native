#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"

class ControllerHistoryTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void aggregateAndTopicWarmNavigationHasNoRedundantHttp()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto a = message(), b = message("B", returnedId, secondTopicId);
        fixture.messages = [=](const Request &request) {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{topic.isEmpty() ? QJsonArray{a, b} : QJsonArray{a}};
        };
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(fixture.controller.projects().size(), 1); fixture.controller.openProject(projectId);
        QTRY_COMPARE(fixture.controller.streams().size(), 1); fixture.controller.selectStream(streamId);
        QTRY_COMPARE(fixture.controller.messages().size(), 2); QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
        fixture.controller.selectTopic(topicId); QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
        QCOMPARE(fixture.aggregateHistoryGets, 1); QCOMPARE(fixture.topicHistoryGets, 1); QCOMPARE(fixture.totalHistoryGets(), 2);
        for (int i = 0; i < 3; ++i) {
            fixture.controller.selectStream(streamId); QTRY_COMPARE(fixture.controller.messages().size(), 2); QTRY_VERIFY(!fixture.controller.busy());
            fixture.controller.selectTopic(topicId); QTRY_COMPARE(fixture.controller.messages().size(), 1); QTRY_VERIFY(!fixture.controller.busy());
        }
        QCOMPARE(fixture.aggregateHistoryGets, 1); QCOMPARE(fixture.topicHistoryGets, 1); QCOMPARE(fixture.totalHistoryGets(), 2);
    }

    void aggregateRefreshTraversalIsBounded_data()
    {
        QTest::addColumn<bool>("large"); QTest::newRow("unique-empty-cursors") << false; QTest::newRow("32MiB-payload") << true;
    }
    void aggregateRefreshTraversalIsBounded()
    {
        QFETCH(bool, large); Fixture fixture; fixture.aggregateMessages = true;
        bool traverse = false; int pages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).hasQueryItem("topic_uuid") || !traverse) return {QJsonArray{historyMessage(1)}};
            ++pages; QJsonArray items;
            if (large) for (int i = 0; i < 30; ++i) {
                auto row = historyMessage(1000 + pages * 30 + i);
                row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", QString(40000, 'x')}}; items.append(row);
            }
            return {items, 200, 0, {{"X-Pagination-Marker", QByteArray::number(pages)}}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        traverse = true; fixture.controller.refresh(); QVERIFY(failed.wait());
        QVERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(pages, large ? 28 : 500); QVERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.messages().size(), 1);
    }

    void aggregateReactionUsesActualTopicAndCanonicalMetadataOnly()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto target = message("B payload", returnedId, secondTopicId); target["reactions"] = QJsonObject{}; target["reaction_users"] = QJsonObject{};
        bool added = false; int mutations = 0;
        const QJsonObject fact{{"uuid", messageId}, {"project_id", projectId}, {"source_name", "native"},
            {"message_uuid", returnedId}, {"user_uuid", userId}, {"emoji_name", "heart"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.reactions = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++mutations; added = true; return {fact}; }
            return {added ? QJsonArray{fact} : QJsonArray{}};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/messages/"))
                return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{message()} : QJsonArray{message(), target}};
            auto canonical = target; canonical["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "old response content must not replace current B"}};
            canonical["reactions"] = QJsonObject{{"heart", 1}}; canonical["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}}; return {canonical};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.toggleReaction(returnedId, "heart"); QTRY_COMPARE(mutations, 1);
        QTRY_VERIFY(!modelMessage(fixture.controller, returnedId).value("reactionBusy").toBool());
        const auto row = modelMessage(fixture.controller, returnedId); QCOMPARE(row.value("content").toString(), QString("B payload"));
        QCOMPARE(row.value("reactions").toList().first().toMap().value("count").toInt(), 1);
        QCOMPARE(row.value("topicId").toString(), secondTopicId);
    }

    void aggregateReadCountersDoNotCancelAnotherTopicConfirmation_data()
    {
        QTest::addColumn<bool>("failLast"); QTest::newRow("both-confirmed") << false; QTest::newRow("last-forbidden") << true;
    }
    void aggregateReadCountersDoNotCancelAnotherTopicConfirmation()
    {
        QFETCH(bool, failLast);
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto a = message("A"); a["read"] = false; auto b = message("B", returnedId, secondTopicId); b["read"] = false;
        int read = 0, streamGets = 0;
        fixture.topics = [](const Request &) {
            auto a = creationTopic(topicId, streamId, "A"), b = creationTopic(secondTopicId, streamId, "B");
            a["unread_count"] = b["unread_count"] = 3;
            a["active_unread_count"] = b["active_unread_count"] = 3; return Response{QJsonArray{a, b}};
        };
        fixture.counters = [&](const Request &request) -> Response {
            if (request.target.path().contains("/streams/")) { ++streamGets; return {QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Channel"}, {"encryption", false}, {"unread_count", 6 - read}, {"active_unread_count", 6 - read},
                {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}}; }
            const auto id = request.target.path().section('/', -1); auto topic = creationTopic(id, streamId, "unchanged name");
            topic["unread_count"] = 2; topic["active_unread_count"] = 2;
            return {topic, 200, id == topicId ? 120 : 0};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                const auto isA = request.target.path().contains(messageId);
                if (!isA && failLast) return {QJsonObject{}, 403};
                ++read; auto result = isA ? a : b; result["read"] = true; return {result};
            }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{a} : QJsonArray{a, b}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId});
        const auto unread = [&](const QString &id) {
            for (const auto &row : fixture.controller.topics()) if (row.toMap().value("id") == id) return row.toMap().value("unread").toInt();
            return -1;
        };
        if (!failLast) QTRY_COMPARE(unread(secondTopicId), 2);
        QTRY_COMPARE(unread(topicId), 2);
        QTRY_COMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), failLast ? 5 : 4);
        QCOMPARE(read, failLast ? 1 : 2); QCOMPARE(streamGets, 1);
    }

    void aggregateReadBatchMakesOneStreamCounterRequestForManyTopics()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        QJsonArray topics, rows; QVariantList visible;
        for (int n = 0; n < 10; ++n) {
            const auto topic = n == 0 ? topicId : historyMessage(1000 + n).value("uuid").toString();
            topics.append(creationTopic(topic, streamId, QString::number(n)));
            auto row = historyMessage(n + 1); row["topic_uuid"] = topic; row["read"] = false; rows.append(row); visible.append(row.value("uuid").toString());
        }
        int streamGets = 0, topicGets = 0, readPosts = 0;
        fixture.topics = [=](const Request &) { return Response{topics}; };
        fixture.counters = [&](const Request &request) -> Response {
            if (request.target.path().contains("/streams/")) { ++streamGets; return {QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Channel"}, {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}}; }
            ++topicGets; return {creationTopic(request.target.path().section('/', -1), streamId, "Topic")};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                ++readPosts; const auto id = request.target.path().split('/').value(6);
                for (const auto &value : rows) if (value.toObject().value("uuid") == id) { auto result = value.toObject(); result["read"] = true; return {result}; }
            }
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid"); QJsonArray result;
            for (const auto &row : rows) if (topic.isEmpty() || row.toObject().value("topic_uuid") == topic) result.append(row);
            return {result};
        };
        // This fixture has ten real topics, not only the default two aliases.
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) {
            if (request.target.path().startsWith("/api/workspace/v1/messenger/stream_topics/")
                && isUuid(request.target.path().section('/', -1))) return fixture.counters(request);
            return fallback(request);
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 10);
        fixture.controller.markVisibleMessagesRead(visible); QTRY_COMPARE(topicGets, 10); QTRY_VERIFY(streamGets > 0);
        QCOMPARE(readPosts, 10); QCOMPARE(streamGets, 1);
    }

    void aggregateDeferredStreamReadRefreshIsFencedByRouteAndSession_data()
    {
        QTest::addColumn<QString>("change"); for (const auto &value : {"route", "logout", "refresh"}) QTest::newRow(value) << QString(value);
    }
    void aggregateDeferredStreamReadRefreshIsFencedByRouteAndSession()
    {
        QFETCH(QString, change); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto a = message("A"); a["read"] = false; auto b = message("B", returnedId, secondTopicId); b["read"] = false;
        int streamGets = 0, bPosts = 0;
        fixture.counters = [&](const Request &request) -> Response {
            if (request.target.path().contains("/streams/")) { ++streamGets; return {QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Channel"}, {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}}; }
            return {creationTopic(request.target.path().section('/', -1), streamId, "Topic")};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                if (request.target.path().contains(returnedId)) { ++bPosts; return {QJsonObject{}, 403, 150}; }
                auto result = a; result["read"] = true; return {result};
            }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{a} : QJsonArray{a, b}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId}); QTRY_COMPARE(bPosts, 1);
        if (change == "route") fixture.controller.selectStream({});
        else if (change == "logout") fixture.controller.logout(); else fixture.session.requestRefresh();
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(streamGets, 0);
    }

    void aggregateTopicDeletionRemovesRowsWithoutAnUnrequestedRefill()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        fixture.messages = [](const Request &request) { return Response{QUrlQuery(request.target).hasQueryItem("topic_uuid")
            ? QJsonArray{message()} : QJsonArray{message(), message("B", returnedId, secondTopicId)}}; };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty()); const auto before = fixture.totalHistoryGets();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", secondTopicId}, {"stream_uuid", streamId}})));
        QCOMPARE(fixture.controller.messages().size(), 1); QVERIFY(modelMessage(fixture.controller, returnedId).isEmpty());
        QCOMPARE(fixture.controller.topics().size(), 1); QCOMPARE(fixture.totalHistoryGets(), before);
        QVERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
    }

    void aggregatePaginationReconcilesLoadedRowsAndRecoversDeletedMarker()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        QMap<int, QJsonObject> rows; int missingMarkers = 0;
        for (int n = 1; n <= 120; ++n) { auto row = historyMessage(n); row["topic_uuid"] = n % 2 ? topicId : secondTopicId; rows[n] = row; }
        fixture.messages = [&](const Request &request) {
            auto scope = rows; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            if (!topic.isEmpty()) for (auto it = scope.begin(); it != scope.end();) {
                if (it->value("topic_uuid") != topic) it = scope.erase(it); else ++it;
            }
            auto response = historyResponse(scope, request); if (response.status == 404) ++missingMarkers; return response;
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 50);
        fixture.controller.loadOlder(); QTRY_COMPARE(fixture.controller.messages().size(), 100);
        auto &edited = rows[30]; edited["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Older B edit"}};
        rows.remove(21); fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 119); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(missingMarkers, 1); QVERIFY(fixture.controller.error().isEmpty());
        QCOMPARE(modelMessage(fixture.controller, historyMessage(30).value("uuid").toString()).value("content").toString(), QString("Older B edit"));
        QVERIFY(modelMessage(fixture.controller, historyMessage(21).value("uuid").toString()).isEmpty());
    }

    void aggregateConfirmedReadPersistsBothPageFamiliesThroughOfflineRestart()
    {
        QTemporaryDir directory; Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto a = message("Saved A"); a["read"] = false; auto b = message("Saved B", returnedId, secondTopicId); b["read"] = false;
        bool offline = false; int reads = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (offline) return {QJsonObject{}, 503};
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; auto result = a; result["read"] = true; return {result}; }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{a} : QJsonArray{a, b}};
        };
        {
            NativeCacheService cache(directory.path(), {}, true); NativeController current(&fixture.api, &fixture.session); current.useCache(&cache);
            current.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId); QTRY_COMPARE(current.streams().size(), 1);
            current.selectStream(streamId); QTRY_COMPARE(current.messages().size(), 2); QTRY_VERIFY(!current.busy()); QVERIFY(flushCache(cache));
            current.selectTopic(topicId); QTRY_COMPARE(current.messages().size(), 1); QTRY_VERIFY(!current.busy()); QVERIFY(flushCache(cache));
            current.markMessagesReadUpTo(messageId); QTRY_VERIFY(modelMessage(current, messageId).value("read").toBool()); QVERIFY(flushCache(cache));
            QCOMPARE(fixture.totalHistoryGets(), 2);
        }
        offline = true;
        fixture.streams = fixture.topics = fixture.users = [](const Request &) { return Response{QJsonObject{}, 503}; };
        NativeCacheService cache(directory.path(), {}, true); ApiClient api(nullptr, true); Session session(&api); NativeController current(&api, &session); current.useCache(&cache);
        current.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId); QTRY_COMPARE(current.streams().size(), 1);
        current.selectStream(streamId); QTRY_COMPARE(current.messages().size(), 2); QTRY_VERIFY(!current.busy());
        QVERIFY(modelMessage(current, messageId).value("read").toBool()); QVERIFY(!modelMessage(current, returnedId).value("read").toBool());
        current.selectTopic(topicId); QTRY_COMPARE(current.messages().size(), 1); QTRY_VERIFY(!current.busy());
        QVERIFY(modelMessage(current, messageId).value("read").toBool()); QVERIFY(current.cacheStale());
        QVERIFY(!current.canSend()); QVERIFY(!modelMessage(current, messageId).value("canReact").toBool());
        QCOMPARE(reads, 1); QCOMPARE(fixture.aggregateHistoryGets, 2); QCOMPARE(fixture.topicHistoryGets, 2);
    }

    void aggregateRejectsMalformedRowsWithoutPartiallyPublishing_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &name : {"project", "stream", "topic", "timestamp", "duplicate"}) QTest::newRow(name) << QString(name);
    }
    void aggregateRejectsMalformedRowsWithoutPartiallyPublishing()
    {
        QFETCH(QString, fault); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; bool invalid = false;
        fixture.messages = [&](const Request &request) -> Response {
            auto row = message(); QJsonArray result{row};
            if (invalid && !QUrlQuery(request.target).hasQueryItem("topic_uuid")) {
                if (fault == "project") row["project_id"] = returnedId;
                if (fault == "stream") row["stream_uuid"] = returnedId;
                if (fault == "topic") row.remove("topic_uuid");
                if (fault == "timestamp") row["created_at"] = "untrusted";
                result.append(row);
            }
            return {result};
        };
        QVERIFY(fixture.start()); invalid = true; fixture.controller.selectTopic({});
        QTRY_VERIFY(!fixture.controller.error().isEmpty()); QVERIFY(fixture.controller.messages().isEmpty());
    }

    void aggregateMutationsVerifyTheActualPlacementIdentity_data()
    {
        QTest::addColumn<QString>("operation"); QTest::addColumn<QString>("fault");
        for (const auto &op : {"edit", "star"}) for (const auto &fault : {"none", "topic", "author"})
            QTest::newRow(qPrintable(QString(op) + '-' + fault)) << QString(op) << QString(fault);
    }
    void aggregateMutationsVerifyTheActualPlacementIdentity()
    {
        QFETCH(QString, operation); QFETCH(QString, fault); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto original = message("Actual B content", returnedId, secondTopicId), canonical = original;
        int mutations = 0, confirmations = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT" || request.method == "POST") {
                ++mutations;
                if (operation == "edit") canonical["payload"] = request.body.value("payload"); else canonical["starred"] = true;
                return {canonical};
            }
            if (request.target.path().endsWith("/messages/")) {
                return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{message()} : QJsonArray{message(), original}};
            }
            ++confirmations; auto reply = canonical;
            if (fault == "topic") reply["topic_uuid"] = topicId;
            if (fault == "author") reply["author_uuid"] = messageId;
            return {reply};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        if (operation == "edit") fixture.controller.editMessage(returnedId, "New B content"); else fixture.controller.starMessage(returnedId);
        QTRY_COMPARE(confirmations, 1); QCOMPARE(mutations, 1);
        if (fault != "none") QTRY_VERIFY(!fixture.controller.error().isEmpty());
        else if (operation == "edit") QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(), QString("New B content"));
        else QTRY_VERIFY(modelMessage(fixture.controller, returnedId).value("starred").toBool());
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("topicId").toString(), secondTopicId); QCOMPARE(row.value("authorId").toString(), userId);
        if (fault != "none") { QCOMPARE(row.value("content").toString(), QString("Actual B content")); QVERIFY(!row.value("starred").toBool()); }
    }

    void aggregateReadFailureContinuesWithAnotherTopic_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"forbidden", "transient", "wrong-topic", "wrong-owner"}) QTest::newRow(fault) << QString(fault);
    }
    void aggregateReadFailureContinuesWithAnotherTopic()
    {
        QFETCH(QString, fault); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto first = message("A"); first["read"] = false;
        auto second = message("B", returnedId, secondTopicId); second["read"] = false;
        int a = 0, b = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                const auto isA = request.target.path().contains(messageId); auto row = isA ? first : second; row["read"] = true;
                if (isA) {
                    ++a;
                    if (fault == "forbidden" || fault == "transient") return {QJsonObject{}, fault == "forbidden" ? 403 : 503};
                    if (fault == "wrong-topic") row["topic_uuid"] = secondTopicId;
                    if (fault == "wrong-owner") row["user_uuid"] = messageId;
                } else ++b;
                return {row};
            }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{first} : QJsonArray{first, second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId});
        QTRY_VERIFY(modelMessage(fixture.controller, returnedId).value("read").toBool());
        QCOMPARE(a, fault == "transient" ? 3 : 1); QCOMPARE(b, 1);
        QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
        fixture.controller.markVisibleMessagesRead({messageId, returnedId});
        QCOMPARE(b, 1); QCOMPARE(a, fault == "transient" ? 3 : 1);
    }

    void aggregateReadHasBoundedInputAndDoesNotOverwriteConcurrentContent()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; int reads = 0;
        auto first = message("Captured A"); first["read"] = false;
        auto second = message("Hidden B", returnedId, secondTopicId); second["read"] = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; auto result = first; result["read"] = true; return {result, 200, 100}; }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{first} : QJsonArray{first, second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVariantList tooMany; for (int i = 0; i < 257; ++i) tooMany.append(messageId);
        fixture.controller.markVisibleMessagesRead(tooMany); QCOMPARE(reads, 0);
        fixture.controller.markVisibleMessagesRead({messageId}); QTRY_COMPARE(reads, 1);
        auto live = first; live["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Newer edited A"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", live)));
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), QString("Newer edited A"));
        QVERIFY(!modelMessage(fixture.controller, returnedId).value("read").toBool());
    }

    void aggregateReadDepartureCancelsOtherTopicIntents()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; int reads = 0;
        auto first = message(); first["read"] = false; auto second = message("B", returnedId, secondTopicId); second["read"] = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; auto result = first; result["read"] = true; return {result, 200, 150}; }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{first} : QJsonArray{first, second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId}); QTRY_COMPARE(reads, 1);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(reads, 1); QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
    }

    void aggregateTopicRailUsesActivityThenMutedAndClosedGroups()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString mutedId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", closedId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        auto old = creationTopic(topicId, streamId, "Z old"), recent = creationTopic(secondTopicId, streamId, "A recent");
        auto muted = creationTopic(mutedId, streamId, "Muted"), closed = creationTopic(closedId, streamId, "Closed");
        old["updated_at"] = "2026-01-01T00:00:00.000001Z"; recent["updated_at"] = "2026-01-02T00:00:00.000001Z";
        muted["updated_at"] = "2026-01-03T00:00:00Z"; muted["notification_mode"] = "mute";
        closed["updated_at"] = "2026-01-04T00:00:00Z"; closed["is_done"] = true;
        fixture.topics = [=](const Request &) { return Response{QJsonArray{closed, muted, old, recent}}; };
        QVERIFY(fixture.start());
        const auto topics = fixture.controller.topics();
        QCOMPARE(topics.at(0).toMap().value("id").toString(), secondTopicId);
        QCOMPARE(topics.at(1).toMap().value("id").toString(), topicId);
        QCOMPARE(topics.at(2).toMap().value("id").toString(), mutedId);
        QCOMPARE(topics.at(3).toMap().value("id").toString(), closedId);
    }

    void aggregateVisibleReadCoalescesSeparatelyForEachActualTopic()
    {
        QStringList notified;
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        QMap<QString, QJsonObject> rows;
        for (int n = 1; n <= 5; ++n) {
            auto row = historyMessage(n); row["topic_uuid"] = n % 2 ? topicId : secondTopicId;
            row["read"] = false; rows.insert(row.value("uuid").toString(), row);
        }
        QList<QString> anchors;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                const auto id = request.target.path().split('/').value(6); anchors.append(id);
                auto anchor = rows.value(id); anchor["read"] = true;
                return {anchor, 200, 30};
            }
            if (!request.target.path().endsWith("/messages/")) return {rows.value(request.target.path().section('/', -1))};
            QJsonArray result; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            for (const auto &row : rows) if (topic.isEmpty() || row.value("topic_uuid") == topic) result.prepend(row);
            return {result};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 5);
        const auto a1 = historyMessage(1).value("uuid").toString();
        const auto a3 = historyMessage(3).value("uuid").toString();
        const auto b2 = historyMessage(2).value("uuid").toString();
        const QVariantList visible{a1, b2, a3};
        fixture.controller.markVisibleMessagesRead(visible); fixture.controller.markVisibleMessagesRead(visible);
        QTRY_COMPARE(anchors.size(), 2);
        QVERIFY(anchors.contains(a3)); QVERIFY(anchors.contains(b2));
        QTRY_VERIFY(modelMessage(fixture.controller, a3).value("read").toBool());
        QTRY_VERIFY(modelMessage(fixture.controller, b2).value("read").toBool());
        QVERIFY(modelMessage(fixture.controller, a1).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, historyMessage(4).value("uuid").toString()).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, historyMessage(5).value("uuid").toString()).value("read").toBool());

        // A queued anchor deleted before dispatch must also disappear from
        // the snapshot last delivered to the visual read-request indicator.
        connect(&fixture.controller, &NativeController::readRequestBoundariesChanged, &fixture.controller,
            [&] { notified = fixture.controller.readRequestBoundaryIds(); });
        const auto a5 = historyMessage(5).value("uuid").toString();
        const auto b4 = historyMessage(4).value("uuid").toString();
        fixture.controller.markMessagesReadUpTo(a5);
        fixture.controller.markMessagesReadUpTo(b4);
        QCOMPARE(notified.size(), 2);
        rows.remove(b4);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted",
            QJsonObject{{"uuid", b4}, {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}})));
        QTRY_VERIFY(fixture.controller.readRequestBoundaryIds().isEmpty());
        QVERIFY(notified.isEmpty());
    }

    void aggregateLoadsActualTopicsAndRealtimeWithoutADefaultComposer()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto first = message("A body"); auto second = message("B body", returnedId, secondTopicId);
        second["created_at"] = "2026-01-02T10:00:00.000001Z";
        int aggregateGets = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonObject{}, 400};
            const auto query = QUrlQuery(request.target);
            if (!query.hasQueryItem("topic_uuid")) { ++aggregateGets; return {QJsonArray{second, first}}; }
            return {QJsonArray{first}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({});
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.controller.property("conversationKind").toString(), QString("stream"));
        QVERIFY(fixture.controller.property("hasConversation").toBool()); QVERIFY(aggregateGets > 0);
        const auto a = modelMessage(fixture.controller, messageId), b = modelMessage(fixture.controller, returnedId);
        QCOMPARE(a.value("topicId").toString(), topicId); QCOMPARE(b.value("topicId").toString(), secondTopicId);
        QCOMPARE(a.value("authorId").toString(), userId); QCOMPARE(b.value("topicName").toString(), QString("Second topic"));
        fixture.controller.setDraft("must not become implicit General draft");
        QVERIFY(!fixture.controller.beginReply(messageId)); // An implicit General stream has no reply slot.
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QVERIFY(fixture.controller.draft().isEmpty()); QVERIFY(!fixture.controller.canSend()); QVERIFY(!fixture.controller.canAttach());
        first["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Live A"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", first)));
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), QString("Live A"));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", creationTopic(secondTopicId, streamId, "Renamed B"))));
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("topicName").toString(), QString("Renamed B"));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}})));
        QCOMPARE(fixture.controller.messages().size(), 1);
    }

    void realtimeAccessChangesStillEraseSavedHistory_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"encryption", "role", "direct_user_uuid"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void realtimeAccessChangesStillEraseSavedHistory()
    {
        QFETCH(QString, field);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache));
        QJsonObject raw{{"uuid", streamId}, {"project_id", projectId}, {"name", "Changed stream"}, {"encryption", false}};
        raw[field] = field == "role" ? QJsonValue("changed-role") : field == "direct_user_uuid" ? QJsonValue(returnedId) : QJsonValue(true);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", raw)));
        QVERIFY(flushCache(cache));
        QVERIFY(!cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}}));
    }

    void realtimeArchiveRetainsVerifiedOrdinaryTopicHistoryAndCapabilities()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useCache(&cache); fixture.controller.useFiles(&files);
        fixture.streams = [](const Request &) { return Response{QJsonArray{creationChannel(streamId, "Native channel")}}; };
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache)); auto &controller = fixture.controller;
        controller.setDraft("Retained ordinary topic"); QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        const auto requests = fixture.server.requests.size();
        // A partial row cannot newly admit retained Archive identity. Existing
        // route and creation guards are tested below with an actual full DTO.
        QVERIFY(!controller.applyRealtimeEvent(controllerEvent("stream.updated",{{"uuid",streamId},
            {"project_id",projectId},{"name","Partial archive"},{"encryption",false},{"is_archived",true}})));
        auto raw=creationChannel(streamId,"Native channel"); raw["is_archived"]=true;
        raw["updated_at"]="2026-01-02T10:02:00Z";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated",raw)));
        QVERIFY(flushCache(cache));
        const auto history=cacheEntry(cache,cache.lease(),{NativeCache::Kind::History,streamId,topicId,{}});
        QVERIFY(history); QVERIFY(!history->fresh); QCOMPARE(history->rows.size(),1);
        QCOMPARE(controller.selectedStream(),streamId); QCOMPARE(controller.selectedTopic(),topicId);
        QCOMPARE(controller.messages().size(),1); QCOMPARE(controller.draft(),QString("Retained ordinary topic"));
        QVERIFY(controller.canSend()); QVERIFY(controller.canAttach()); QVERIFY(!controller.canCreateTopicIn(streamId));
        controller.createTopic(streamId,"Still archived");
        QVERIFY(controller.streams().isEmpty()); QCOMPARE(controller.archivedChats().size(),1);
        for (int n=requests;n<fixture.server.requests.size();++n) QCOMPARE(fixture.server.requests[n].method,QByteArray("GET"));
    }

    void messageCreatedAtUsesTheValidatedUtcDisplayTimestamp()
    {
        Fixture fixture;
        auto row = message("Timestamp"); row["created_at"] = "2026-11-06T15:34:56.123456+03:00";
        fixture.messages = [row](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("createdAt").toString(),
                 QString("2026-11-06T12:34:56.123Z"));
    }

    void canonicalMessageConfirmationPreservesNewerFieldRevisions_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"edit-aba", "star-aba", "edit-other-star", "star-other-payload",
                                    "edit-delete", "star-delete", "send-delete"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void canonicalMessageConfirmationPreservesNewerFieldRevisions()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        auto row = message("Payload A"); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int confirmations = 0, history = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/star/invoke")) { row["starred"] = true; return Response{row}; }
            if (request.method == "PUT") { row["payload"] = request.body.value("payload"); return Response{row}; }
            if (request.method == "POST") {
                row = message(request.body.value("payload").toObject().value("content").toString(), request.body.value("uuid").toString());
                return Response{row, 201};
            }
            if (request.target.path().endsWith("/messages/")) { ++history; return Response{QJsonArray{row}}; }
            ++confirmations; return Response{row, 200, 200};
        };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache));
        if (scenario.startsWith("edit")) fixture.controller.editMessage(messageId, "Payload B");
        else if (scenario.startsWith("star")) fixture.controller.starMessage(messageId);
        else { fixture.controller.setDraft("Payload B"); fixture.controller.send(); }
        QTRY_COMPARE(confirmations, 1);
        const auto id = row.value("uuid").toString();
        if (scenario.endsWith("delete")) {
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{
                {"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}})));
        } else if (scenario == "edit-aba") {
            auto updated = message("Payload C");
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 12)));
            updated["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Payload A"}};
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 13)));
            QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "Payload A");
        } else if (scenario == "star-aba") {
            auto updated = message("Payload A"); updated["starred"] = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 12)));
            updated["starred"] = false;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 13)));
            QVERIFY(!modelMessage(fixture.controller, messageId).value("starred").toBool());
        } else if (scenario == "edit-other-star") {
            auto updated = message("Payload A"); updated["starred"] = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated)));
        } else {
            auto updated = message("Payload C"); updated["starred"] = false;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated)));
        }
        // Wait for the actual ApiClient callback/deferred reply destruction,
        // rather than using server socket lifetime as client completion.
        QTRY_COMPARE(fixture.api.findChildren<QNetworkReply *>().size(), 0);
        if (scenario.endsWith("delete")) QVERIFY(modelMessage(fixture.controller, id).isEmpty());
        else if (scenario == "edit-aba") QCOMPARE(modelMessage(fixture.controller, id).value("content").toString(), "Payload A");
        else if (scenario == "star-aba") QVERIFY(!modelMessage(fixture.controller, id).value("starred").toBool());
        else {
            QCOMPARE(modelMessage(fixture.controller, id).value("content").toString(), scenario == "edit-other-star" ? "Payload B" : "Payload C");
            QVERIFY(modelMessage(fixture.controller, id).value("starred").toBool());
        }
        QCOMPARE(confirmations, 1); QCOMPARE(history, 1);
        QVERIFY(flushCache(cache));
        const auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        if (scenario == "send-delete") {
            QVERIFY(id != messageId);
            QVERIFY(saved && !saved->fresh);
            QCOMPARE(saved->rows.size(), 1);
            const auto original = saved->rows.first().toObject();
            QCOMPARE(original.value("uuid").toString(), messageId);
            QCOMPARE(original.value("payload").toObject().value("content").toString(), QStringLiteral("Payload A"));
        } else if (scenario.endsWith("delete")) QVERIFY(!saved);
        else {
            QVERIFY(saved && !saved->fresh);
            const auto raw = saved->rows.first().toObject();
            QCOMPARE(raw.value("payload").toObject().value("content").toString(),
                     modelMessage(fixture.controller, messageId).value("content").toString());
            QCOMPARE(raw.value("starred").toBool(), modelMessage(fixture.controller, messageId).value("starred").toBool());
        }
    }

    void confirmedNonReadMutationSurvivesRestartWithoutAnotherHistoryGet_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("catalogsOffline");
        for (const auto *operation : {"send", "edit", "star", "reaction-add", "reaction-remove", "delete"})
            QTest::newRow(operation) << QString::fromLatin1(operation) << false;
        QTest::newRow("send-all-messenger-offline") << QString("send") << true;
        QTest::newRow("edit-all-messenger-offline") << QString("edit") << true;
        for (const auto *event : {"rt-message-created", "rt-message-updated", "rt-stream-updated", "rt-topic-updated"})
            QTest::newRow(event) << QString::fromLatin1(event) << true;
    }
    void confirmedNonReadMutationSurvivesRestartWithoutAnotherHistoryGet()
    {
        QFETCH(QString, operation); QFETCH(bool, catalogsOffline);
        QTemporaryDir directory;
        Fixture transport;
        QMap<QString, QJsonObject> rows{{messageId, message("Previously saved message")}};
        bool restoring = false, own = operation == "reaction-remove";
        int historyGets = 0, canonicalGets = 0, mutations = 0;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"},
            {"updated_at", "2026-01-02T10:00:00Z"}};
        const auto aggregate = [&] {
            rows[messageId]["reactions"] = own ? QJsonObject{{"heart", 1}} : QJsonObject{};
            rows[messageId]["reaction_users"] = own ? QJsonObject{{"heart", QJsonArray{userId}}} : QJsonObject{};
        };
        aggregate();
        transport.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/star/invoke")) {
                ++mutations; rows[messageId]["starred"] = true; return Response{rows[messageId]};
            }
            if (request.method == "DELETE") {
                ++mutations; rows.remove(messageId); return Response{QJsonObject{}, 204};
            }
            if (request.method == "PUT") {
                ++mutations; rows[messageId]["payload"] = request.body.value("payload"); return Response{rows[messageId]};
            }
            if (request.method == "POST") {
                ++mutations; const auto id = request.body.value("uuid").toString();
                auto sent = message(request.body.value("payload").toObject().value("content").toString(), id);
                sent["created_at"] = "2026-01-02T10:00:01.000001Z"; rows.insert(id, sent);
                return Response{sent, 201};
            }
            if (path.endsWith("/messages/")) {
                ++historyGets;
                if (restoring) return Response{QJsonObject{}, 503};
                QJsonArray values; for (const auto &row : rows) values.append(row);
                return Response{values};
            }
            ++canonicalGets;
            return Response{rows.value(path.section('/', -1))};
        };
        transport.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; own = true; aggregate(); return Response{fact, 201}; }
            if (request.method == "DELETE") { ++mutations; own = false; aggregate(); return Response{QJsonObject{}, 204}; }
            return Response{own ? QJsonArray{fact} : QJsonArray{}};
        };
        const auto enter = [&](NativeController &controller) {
            controller.login(transport.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            if (!QTest::qWaitFor([&] { return controller.projects().size() == 1; }, 2000)) return false;
            controller.openProject(projectId);
            if (!QTest::qWaitFor([&] { return controller.streams().size() == 1; }, 2000)) return false;
            controller.selectStream(streamId);
            if (!QTest::qWaitFor([&] { return controller.topics().size() == 1; }, 2000)) return false;
            controller.selectTopic(topicId);
            return QTest::qWaitFor([&] { return !controller.busy(); }, 2000);
        };
        QString sentId;
        {
            NativeCacheService cache(directory.path(), {}, true);
            ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
            controller.useCache(&cache); QVERIFY(enter(controller)); QVERIFY(flushCache(cache));
            QCOMPARE(historyGets, 1);
            if (operation.startsWith("rt-")) {
                const auto kind = operation.mid(3);
                QJsonObject raw;
                if (kind == "message-created") {
                    sentId = returnedId;
                    raw = message("Saved realtime insertion", sentId); raw["created_at"] = "2026-01-02T10:00:01Z";
                } else if (kind == "message-updated") {
                    raw = rows[messageId]; raw["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Saved realtime edit"}};
                } else if (kind == "stream-updated") {
                    raw = {{"uuid", streamId}, {"project_id", projectId}, {"name", "Renamed live channel"},
                        {"encryption", false}, {"unread_count", 1}};
                } else {
                    raw = {{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Renamed live topic"}};
                }
                QString eventKind = kind; eventKind.replace('-', '.');
                QVERIFY(controller.applyRealtimeEvent(controllerEvent(eventKind, raw)));
            } else if (operation == "send") {
                QSignalSpy sent(&controller, &NativeController::messageSent);
                controller.setDraft("Saved own send"); controller.send(); QTRY_COMPARE(sent.size(), 1);
                sentId = sent.first().first().toString();
                QCOMPARE(modelMessage(controller, sentId).value("content").toString(), "Saved own send");
            } else if (operation == "edit") {
                controller.editMessage(messageId, "Saved confirmed edit");
                QTRY_COMPARE(modelMessage(controller, messageId).value("content").toString(), "Saved confirmed edit");
            } else if (operation == "delete") {
                controller.deleteMessage(messageId); QTRY_VERIFY(controller.messages().isEmpty());
            } else if (operation == "star") {
                controller.starMessage(messageId); QTRY_VERIFY(modelMessage(controller, messageId).value("starred").toBool());
            } else {
                controller.toggleReaction(messageId, "heart");
                QTRY_VERIFY(!modelMessage(controller, messageId).value("reactionBusy").toBool());
                QCOMPARE(modelMessage(controller, messageId).value("reactions").toList().size(), own ? 1 : 0);
            }
            QCOMPARE(mutations, operation.startsWith("rt-") ? 0 : 1);
            QCOMPARE(canonicalGets, operation == "delete" || operation.startsWith("rt-") ? 0 : 1); QCOMPARE(historyGets, 1);
            QVERIFY(flushCache(cache));
        }
        restoring = true;
        if (catalogsOffline) {
            transport.streams = transport.topics = transport.users = [](const Request &) { return Response{QJsonObject{}, 503}; };
        }
        NativeCacheService cache(directory.path(), {}, true);
        ApiClient api(nullptr, true); Session session(&api); NativeController restored(&api, &session);
        restored.useCache(&cache); QVERIFY(enter(restored));
        QTRY_COMPARE(restored.messages().size(), operation == "send" || operation == "rt-message-created" ? 2 : operation == "delete" ? 0 : 1);
        const auto row = modelMessage(restored, messageId);
        if (operation != "delete")
            QCOMPARE(row.value("content").toString(), operation == "edit" ? "Saved confirmed edit" : operation == "rt-message-updated" ? "Saved realtime edit" : "Previously saved message");
        if (operation == "send" || operation == "rt-message-created")
            QCOMPARE(modelMessage(restored, sentId).value("content").toString(), operation == "send" ? "Saved own send" : "Saved realtime insertion");
        if (operation == "star") QVERIFY(row.value("starred").toBool());
        if (operation.startsWith("reaction-")) QCOMPARE(row.value("reactions").toList().size(), own ? 1 : 0);
        if (operation != "delete") QVERIFY(restored.cacheStale());
        restored.setDraft("Local draft awaiting revalidation");
        QVERIFY(!restored.canSend()); QVERIFY(!restored.canLoadOlder()); QVERIFY(!row.value("canReact").toBool());
        QCOMPARE(historyGets, 2);
        QCOMPARE(canonicalGets, operation == "delete" || operation.startsWith("rt-") ? 0 : 1);
        QCOMPARE(mutations, operation.startsWith("rt-") ? 0 : 1);
    }

    void cacheStatusObserversMayDestroyTheController_data()
    {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"opening", "logout", "replacement", "cache-destroyed"})
            QTest::newRow(phase) << QString::fromLatin1(phase);
    }
    void cacheStatusObserversMayDestroyTheController()
    {
        QFETCH(QString, phase);
        QTemporaryDir directory; Fixture fixture;
        QPointer<NativeCacheService> cache = new NativeCacheService(directory.path(), {}, true);
        NativeCacheService replacement(directory.filePath("replacement"), {}, true);
        QPointer<NativeController> controller = new NativeController(&fixture.api, &fixture.session);
        controller->useCache(cache);
        if (phase == "opening") connect(cache, &NativeCacheService::stateChanged, this, [&] {
            if (controller && cache->maintenanceBusy()) delete controller.data();
        });
        controller->login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller->projects().size(), 1); controller->openProject(projectId);
        if (phase == "opening") QTRY_VERIFY(!controller);
        else {
            QTRY_VERIFY(cache->available()); QTRY_VERIFY(!controller->busy());
            if (phase == "cache-destroyed") {
                connect(controller, &NativeController::stateChanged, this, [&] { delete controller.data(); });
                delete cache.data();
            } else {
                connect(cache, &NativeCacheService::stateChanged, this, [&] {
                    if (controller && !cache->available()) delete controller.data();
                });
                if (phase == "logout") controller->logout();
                else controller->useCache(&replacement);
            }
            QVERIFY(!controller);
            if (phase == "logout") QVERIFY(!fixture.session.ready());
        }
        if (cache) delete cache.data();
    }

    void savedAccountForgetRequiresItsCurrentDurableCleanup_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"success", "cleanup-error", "destroy-pending", "replace-pending", "destroy-before-admission", "session-changed"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void savedAccountForgetRequiresItsCurrentDurableCleanup()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory; ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const QUrl server("https://cache-forget.invalid");
        SavedAccount account{SessionVault::accountId(server, projectId, userId), server, projectId, userId,
                             "synthetic-user", "Synthetic project"};
        bool saved = false;
        vault.save(account, "synthetic-refresh", [&](const QString &error) { QVERIFY(error.isEmpty()); saved = true; });
        QTRY_VERIFY(saved);
        ApiClient api; Session session(&api); session.setVault(&vault);
        NativeController controller(&api, &session);
        QPointer<NativeCacheService> cache = new NativeCacheService(directory.filePath("cache"));
        const auto cleanup = qScopeGuard([&] { if (cache) delete cache.data(); });
        NativeCacheService replacement(directory.filePath("cache"));
        QVERIFY(cache->open(server, projectId, userId)); QTRY_VERIFY(cache->available());
        bool written = false;
        QVERIFY(cache->put(cache->lease(), {NativeCache::Kind::History, streamId, topicId, {}}, QJsonArray{message()}, {}, this,
                           [&](bool okay) { written = okay; }));
        QTRY_VERIFY(written); controller.useCache(cache);
        if (scenario == "destroy-before-admission") {
            connect(&controller, &NativeController::stateChanged, this, [&] { if (cache && controller.busy()) delete cache.data(); });
            controller.forgetSavedAccount(account.id);
        } else {
            const auto connection = QUuid::createUuid().toString();
            {
                auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
                database.setDatabaseName(directory.filePath("cache/cache.sqlite3")); QVERIFY(database.open());
                QSqlQuery writer(database); QVERIFY(writer.exec("BEGIN IMMEDIATE"));
                controller.forgetSavedAccount(account.id);
                QVERIFY(controller.busy()); QCOMPARE(vault.accounts().size(), 1);
                if (scenario == "destroy-pending") {
                    QTimer::singleShot(0, this, [&] { QVERIFY(writer.exec("ROLLBACK")); });
                    delete cache.data();
                } else if (scenario == "replace-pending") {
                    controller.useCache(&replacement); QVERIFY(writer.exec("ROLLBACK"));
                } else if (scenario == "session-changed") {
                    session.logout();
                    QVERIFY(!controller.busy());
                    QVERIFY(writer.exec("ROLLBACK"));
                } else if (scenario == "success") QVERIFY(writer.exec("ROLLBACK"));
                if (cache && (scenario == "session-changed" || scenario == "replace-pending"))
                    QVERIFY(flushCache(*cache));
                QTRY_VERIFY(!controller.busy());
                if (scenario == "success") QTRY_VERIFY(vault.accounts().isEmpty());
                else {
                    QCOMPARE(vault.accounts().size(), 1);
                    if (scenario == "cleanup-error") QVERIFY(!controller.error().isEmpty());
                }
                database.close();
            }
            QSqlDatabase::removeDatabase(connection);
        }
        QVERIFY(!controller.busy());
        if (scenario != "success") QCOMPARE(vault.accounts().size(), 1);
        if (cache) delete cache.data();
    }

    void unrelatedCacheRevisionDuringWarmTopicsLookupFallsBackToRest_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"same-route", "left-route", "signed-out"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void unrelatedCacheRevisionDuringWarmTopicsLookupFallsBackToRest()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        QSemaphore gate;
        std::atomic_bool hold{false}, entered{false};
        NativeCacheService cache(directory.path(), {}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return 1000;
        });
        Fixture fixture; fixture.controller.useCache(&cache);
        int topics = 0;
        fixture.topics = [&](const Request &) {
            ++topics;
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Current topic"}}}};
        };
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache)); QCOMPARE(topics, 1);
        hold = true;
        fixture.controller.selectStream({}); fixture.controller.selectStream(streamId);
        QTRY_VERIFY(entered.load());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read",
            QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{messageId}}})));
        if (scenario == "left-route") fixture.controller.selectStream({});
        else if (scenario == "signed-out") fixture.controller.logout();
        gate.release();
        if (scenario == "same-route") {
            QTRY_COMPARE(fixture.controller.topics().size(), 1);
            QCOMPARE(topics, 2);
        } else {
            QVERIFY(flushCache(cache));
            QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
            QVERIFY(fixture.controller.topics().isEmpty());
            QCOMPARE(topics, 1);
        }
    }

    void destroyedCacheDuringWarmHistoryLookupFallsBackToRest()
    {
        QTemporaryDir directory; QSemaphore gate;
        std::atomic_bool hold{false}, entered{false};
        QPointer<NativeCacheService> cache = new NativeCacheService(directory.path(), {}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return 1000;
        });
        const auto cleanup = qScopeGuard([&] { if (cache) delete cache.data(); });
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(cache);
        int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history;
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("network after cache loss", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QVERIFY(flushCache(*cache));
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(flushCache(*cache)); QCOMPARE(history, 2);
        hold = true; fixture.controller.selectTopic(topicId); QTRY_VERIFY(entered.load());
        QTimer::singleShot(0, this, [&] { gate.release(); });
        delete cache.data();
        QTRY_COMPARE(history, 3);
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "network after cache loss");
        QVERIFY(!fixture.controller.cacheStale());
    }

    void obsoleteWarmHistoryReplyCannotStartReadsBeforeRenewalIsVerified()
    {
        QTemporaryDir directory; QSemaphore gate;
        std::atomic_bool hold{false}, entered{false};
        NativeCacheService cache(directory.path(), {}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return 1000;
        });
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        bool renewing = false, verified = false; int premature = 0, history = 0;
        fixture.authentication = [&](const Request &request) {
            if (renewing && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-cache-generation-token"},
                    {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            auto response = sessionResponse(request);
            if (renewing && request.target.path().endsWith("/actions/introspect")) {
                response.delay = 250;
                gate.release();
            }
            return response;
        };
        fixture.messages = [&](const Request &request) {
            ++history;
            if (renewing && !verified) ++premature;
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("verified page", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache));
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(flushCache(cache)); QCOMPARE(history, 2);
        hold = true; fixture.controller.selectTopic(topicId); QTRY_VERIFY(entered.load());
        connect(&fixture.session, &Session::authenticated, this, [&] { verified = true; });
        renewing = true; fixture.session.requestRefresh();
        QTRY_VERIFY(verified); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(premature, 0);
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "verified page");
    }

    void delayedCacheOpenHydratesAfterQuickCatalogFailuresWithoutRetryingRest()
    {
        QTemporaryDir directory; Fixture fixture;
        {
            NativeCache seed(directory.path(),{},true); QVERIFY(seed.open(fixture.server.origin(),projectId,userId));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Streams,{},{},{}},QJsonArray{QJsonObject{{"uuid",streamId},{"project_id",projectId},{"name","Saved channel"},{"encryption",false}}}));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Users,{},{},{}},QJsonArray{QJsonObject{{"uuid",userId},{"username","saved-owner"}}}));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Topics,streamId,{},{}},QJsonArray{QJsonObject{{"uuid",topicId},{"project_id",projectId},{"stream_uuid",streamId},{"name","Saved topic"}}}));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::History,streamId,topicId,{}},QJsonArray{message("Saved while messenger is unavailable")}));
        }
        int streams=0,users=0,topics=0,history=0;
        fixture.streams=[&](const Request &){++streams;return Response{QJsonObject{},503};};
        fixture.users=[&](const Request &){++users;return Response{QJsonObject{},503};};
        fixture.topics=[&](const Request &){++topics;return Response{QJsonObject{},503};};
        fixture.messages=[&](const Request &){++history;return Response{QJsonObject{},503};};
        NativeCacheService cache(directory.path(),{},true); fixture.controller.useCache(&cache);
        const auto connection=QUuid::createUuid().toString();
        {
            auto database=QSqlDatabase::addDatabase("QSQLITE",connection); database.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(database.open());
            QSqlQuery writer(database); QVERIFY(writer.exec("BEGIN IMMEDIATE")); bool released=false;
            QObject releaseContext;
            connect(&fixture.controller,&NativeController::stateChanged,&releaseContext,[&]{
                if(!released && !fixture.controller.error().isEmpty() && cache.maintenanceBusy()){
                    QVERIFY(writer.exec("ROLLBACK"));released=true;
                }
            });
            fixture.controller.login(fixture.server.origin().toString(),"synthetic-user","synthetic-password","");
            QTRY_COMPARE(fixture.controller.projects().size(),1);fixture.controller.openProject(projectId);
            QTRY_VERIFY(released);QTRY_COMPARE(fixture.controller.streams().size(),1);
            QCOMPARE(fixture.controller.streams().first().toMap().value("name").toString(),"Saved channel");
            fixture.controller.selectStream(streamId);QTRY_COMPARE(fixture.controller.topics().size(),1);
            fixture.controller.selectTopic(topicId);QTRY_COMPARE(fixture.controller.messages().size(),1);
            QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
            QCOMPARE(modelMessage(fixture.controller,messageId).value("content").toString(),"Saved while messenger is unavailable");
            QVERIFY(fixture.controller.cacheStale());QVERIFY(!fixture.controller.canSend());QVERIFY(!fixture.controller.canLoadOlder());
            QCOMPARE(streams,1);QCOMPARE(users,1);QCOMPARE(topics,1);QCOMPARE(history,1);database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void pendingResizeCannotDelayAuthoritativeNavigation()
    {
        QTemporaryDir directory; QSemaphore gate; std::atomic_bool hold{false},entered{false};
        NativeCacheService cache(directory.path(),{},true,[&]{if(hold.exchange(false)){entered=true;gate.tryAcquire(1,2000);}return 1000;});
        Fixture fixture;fixture.extraTopic=true;fixture.controller.useCache(&cache);int history=0;
        fixture.messages=[&](const Request &request){++history;const auto topic=QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("Network remains responsive",topic==topicId?messageId:returnedId,topic)}};};
        QVERIFY(fixture.start());QVERIFY(flushCache(cache));hold=true;
        QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::History,streamId,topicId,"blocked"},QJsonArray{message()}));QTRY_VERIFY(entered.load());
        QVERIFY(cache.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));QVERIFY(!cache.available());
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(modelMessage(fixture.controller,returnedId).value("content").toString(),"Network remains responsive");
        QVERIFY(cache.maintenanceBusy());QCOMPARE(history,2);gate.release();QTRY_VERIFY(cache.available());QVERIFY(flushCache(cache));QCOMPARE(history,2);
    }

    void reopenTopicConfirmsWithoutRepeatingToggle_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"success", "forbidden", "ambiguous-applied", "ambiguous-unapplied",
                "invalid-done", "wrong-topic", "wrong-project", "failed-confirmation", "other-route", "logout", "newer-live-close"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void reopenTopicConfirmsWithoutRepeatingToggle()
    {
        QFETCH(QString, scenario);
        Fixture fixture;
        fixture.extraTopic = true;
        bool done = true;
        bool validToggleRequest = true;
        int mutations = 0, confirmations = 0;
        const auto topic = [&] {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                               {"name", "Resolved topic"}, {"is_done", done}};
        };
        fixture.topics = [&](const Request &) {
            return Response{QJsonArray{topic(), QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Another topic"}, {"is_done", false}}}};
        };
        fixture.topicActions = [&](const Request &request) {
            validToggleRequest = validToggleRequest && request.method == "POST" && request.body.isEmpty();
            ++mutations;
            if (scenario != "forbidden" && scenario != "ambiguous-unapplied") done = false;
            return Response{QJsonObject{}, scenario == "forbidden" ? 403
                : scenario.startsWith("ambiguous-") ? 503 : 200, 80};
        };
        fixture.counters = [&](const Request &request) {
            if (request.target.path().endsWith(topicId)) {
                ++confirmations;
                auto value = topic();
                if (scenario == "invalid-done") value["is_done"] = "false";
                if (scenario == "wrong-topic") value["uuid"] = secondTopicId;
                if (scenario == "wrong-project") value["project_id"] = returnedId;
                return Response{value, scenario == "failed-confirmation" ? 503 : 200,
                    scenario == "newer-live-close" ? 150 : 0};
            }
            return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Draft survives reopening");
        QVERIFY(fixture.controller.topicClosed());
        QVERIFY(fixture.controller.canReopenTopic());
        fixture.controller.reopenTopic();
        QVERIFY(!fixture.controller.canReopenTopic());
        fixture.controller.reopenTopic();
        QTRY_COMPARE(mutations, 1);
        QVERIFY(validToggleRequest);
        if (scenario == "other-route") fixture.controller.selectTopic(secondTopicId);
        if (scenario == "logout") fixture.controller.logout();
        if (scenario == "newer-live-close") {
            QTRY_COMPARE(confirmations, 1);
            done = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topic())));
        }
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(mutations, 1);
        if (scenario == "other-route" || scenario == "logout") {
            QCOMPARE(confirmations, 0);
            QCOMPARE(fixture.controller.selectedTopic(), scenario == "logout" ? QString{} : secondTopicId);
            QVERIFY(!fixture.controller.topicClosed());
            return;
        }
        QCOMPARE(confirmations, 1);
        QCOMPARE(fixture.controller.draft(), QString("Draft survives reopening"));
        const bool confirmed = scenario == "success" || scenario == "ambiguous-applied";
        QCOMPARE(fixture.controller.topicClosed(), !confirmed);
        QCOMPARE(fixture.controller.canSend(), confirmed);
        if (confirmed) {
            QVERIFY(fixture.controller.error().isEmpty());
            fixture.controller.reopenTopic();
            QCOMPARE(mutations, 1);
        } else {
            if (scenario == "newer-live-close") {
                QVERIFY(fixture.controller.error().isEmpty());
                // The superseding event starts a separate authoritative topics
                // refresh; ending the toggle does not complete that GET.
                QTRY_VERIFY(fixture.controller.canReopenTopic());
            } else QVERIFY(!fixture.controller.error().isEmpty());
            if (scenario != "forbidden" && scenario != "ambiguous-unapplied" && scenario != "newer-live-close") {
                QVERIFY(!fixture.controller.canReopenTopic());
                fixture.controller.reopenTopic();
                QCOMPARE(mutations, 1);
            }
        }
    }

    void ownershipMatchesTheSourceViewerFallback_data()
    {
        QTest::addColumn<bool>("authorIsViewer");
        QTest::addColumn<bool>("serverOwn");
        for (const bool authorIsViewer : {false, true})
            for (const bool serverOwn : {false, true})
                QTest::newRow(qPrintable(QString("author-%1-flag-%2").arg(authorIsViewer).arg(serverOwn)))
                    << authorIsViewer << serverOwn;
    }
    void ownershipMatchesTheSourceViewerFallback()
    {
        QFETCH(bool, authorIsViewer);
        QFETCH(bool, serverOwn);
        Fixture fixture;
        auto row = message();
        row["author_uuid"] = authorIsViewer ? userId : returnedId;
        row["is_own"] = serverOwn;
        int edits = 0, removals = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "PUT") {
                ++edits;
                row["payload"] = request.body.value("payload");
                return Response{row};
            }
            if (request.method == "DELETE") { ++removals; return Response{QJsonObject{}}; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start());
        const bool own = authorIsViewer || serverOwn;
        QCOMPARE(modelMessage(fixture.controller, messageId).value("own").toBool(), own);
        fixture.controller.editMessage(messageId, "Changed by the viewer");
        if (own) {
            QTRY_COMPARE(edits, 1);
            QTRY_COMPARE(modelMessage(fixture.controller, messageId).value("content").toString(),
                         QString("Changed by the viewer"));
        }
        fixture.controller.deleteMessage(messageId);
        if (own) { QTRY_COMPARE(removals, 1); QTRY_VERIFY(fixture.controller.messages().isEmpty()); }
        else { QCOMPARE(edits, 0); QCOMPARE(removals, 0); QCOMPARE(fixture.controller.messages().size(), 1); }
    }

    void reactionParticipantNamesUseTheCurrentVerifiedDirectory()
    {
        Fixture fixture;
        fixture.users = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"name", "Synthetic Owner"}}}};
        };
        fixture.messages = [](const Request &) {
            auto row = message();
            row["reactions"] = QJsonObject{{"heart", 3}};
            row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId, returnedId}}};
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        const auto participants = [&] {
            return fixture.controller.messages().first().toMap().value("reactions").toList()
                .first().toMap().value("participants").toList();
        };
        QTRY_COMPARE(participants().value(0).toMap().value("name").toString(), QString("Synthetic Owner"));
        QCOMPARE(participants().size(), 2);
        QCOMPARE(participants().at(1).toMap(), QVariantMap({{"id", returnedId}, {"name", "#77777777"}}));
        const QJsonObject renamed{{"uuid", userId}, {"username", "renamed"},
            {"first_name", "Updated"}, {"last_name", "Owner"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", renamed)));
        QCOMPARE(participants().first().toMap().value("name").toString(), QString("Updated Owner"));
        QCOMPARE(fixture.controller.messages().first().toMap().value("reactions").toList().first()
                     .toMap().value("count").toInt(), 3);
        fixture.controller.logout();
        QVERIFY(fixture.controller.messages().isEmpty());
        fixture.users = [](const Request &) { return Response{QJsonArray{}}; };
        QVERIFY(fixture.start());
        QTRY_COMPARE(participants().first().toMap().value("name").toString(), QString("#22222222"));
    }

    void closedTopicsBlockNewSendsAndUploadsWithoutDiscardingDrafts_data()
    {
        QTest::addColumn<QString>("transition");
        for (const auto *state : {"initially-closed", "realtime-closed", "realtime-reopened", "other-topic-closed"})
            QTest::newRow(state) << QString::fromLatin1(state);
    }

    void closedTopicsBlockNewSendsAndUploadsWithoutDiscardingDrafts()
    {
        QFETCH(QString, transition);
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const auto topic = [](const QString &id, bool done) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Synthetic topic"}, {"is_done", done}};
        };
        fixture.topics = [&](const Request &) {
            return Response{QJsonArray{topic(topicId, transition == "initially-closed"), topic(secondTopicId, false)}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Keep this draft until the conversation reopens");
        if (transition != "initially-closed") {
            const auto affected = transition == "other-topic-closed" ? secondTopicId : topicId;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topic(affected, true))));
        }
        if (transition == "realtime-reopened")
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topic(topicId, false), 13)));
        const bool closed = transition == "initially-closed" || transition == "realtime-closed";
        QCOMPARE(fixture.controller.canSend(), !closed);
        QCOMPARE(fixture.controller.canAttach(), !closed);
        if (closed) {
            const auto before = fixture.server.requests.size();
            fixture.controller.send();
            fixture.controller.attachFiles({QUrl::fromLocalFile("/nonexistent/cassi-closed-topic")});
            QCOMPARE(fixture.server.requests.size(), before);
            QVERIFY(!fixture.controller.fileBusy());
        }
        QCOMPARE(fixture.controller.draft(), "Keep this draft until the conversation reopens");
    }

    void reactionAddedReportsOnlyVerifiedNewLocalAdditions_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"verified", "remove", "already-owned", "uncertain-replay", "route-replay", "generation-replay", "rejected", "realtime", "logout-on-notify", "route-on-notify"})
            QTest::newRow(operation) << QString::fromLatin1(operation);
    }
    void reactionAddedReportsOnlyVerifiedNewLocalAdditions()
    {
        QFETCH(QString, operation);
        Fixture fixture; fixture.extraTopic = true;
        bool applied = operation == "remove" || operation == "already-owned";
        bool rotated = false; int adds = 0, removes = 0;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId}, {"user_uuid", userId},
            {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        const auto snapshot = [&] {
            const bool aggregate = applied && operation != "already-owned";
            auto row = message(); row["reactions"] = aggregate ? QJsonObject{{"heart", 1}} : QJsonObject{};
            row["reaction_users"] = aggregate ? QJsonObject{{"heart", QJsonArray{userId}}} : QJsonObject{}; return row;
        };
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith('/') && QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{message("other route", returnedId, secondTopicId)}};
            return Response{request.target.path().endsWith('/') ? QJsonValue(QJsonArray{snapshot()}) : QJsonValue(snapshot())};
        };
        fixture.authentication = [&](const Request &request) {
            if (rotated && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-reaction-added-rotated"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                ++adds;
                if (operation == "rejected") return Response{QJsonObject{}, 403};
                if (operation == "uncertain-replay") return Response{QJsonObject{}, 503};
                applied = true; return Response{fact, 201, operation == "route-replay" || operation == "generation-replay" ? 350 : 0};
            }
            if (request.method == "DELETE") { ++removes; applied = false; return Response{QJsonObject{}, 204}; }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); QSignalSpy added(&fixture.controller, &NativeController::reactionAdded);
        bool handledNotification = false;
        if (operation.endsWith("on-notify")) connect(&fixture.controller, &NativeController::messagesChanged, &fixture.controller, [&] {
            const auto row = modelMessage(fixture.controller, messageId);
            if (handledNotification || row.value("reactionBusy").toBool() || row.value("reactions").toList().isEmpty()) return;
            handledNotification = true;
            if (operation == "logout-on-notify") fixture.controller.logout();
            else fixture.controller.selectTopic(secondTopicId);
        });
        if (operation == "realtime") {
            applied = true; QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", snapshot())));
        } else {
            fixture.controller.toggleReaction(messageId, operation == "verified" ? QString::fromUtf8("❤️") : "heart");
            if (operation == "route-replay") {
                QTRY_COMPARE(adds, 1); fixture.controller.selectTopic(secondTopicId);
                QTRY_VERIFY(!fixture.controller.busy()); fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
                QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
                fixture.controller.toggleReaction(messageId, "heart");
            } else if (operation == "generation-replay") {
                QTRY_COMPARE(adds, 1); rotated = true;
                QSignalSpy renewed(&fixture.session, &Session::authenticated); fixture.session.requestRefresh();
                QTRY_COMPARE(renewed.size(), 1); QTRY_VERIFY(!fixture.controller.busy());
                fixture.controller.toggleReaction(messageId, "heart");
            } else if (operation == "uncertain-replay") {
                QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
                applied = true; fixture.controller.toggleReaction(messageId, "heart");
            }
            QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        }
        const bool expected = operation == "verified";
        QCOMPARE(added.size(), expected ? 1 : 0);
        if (expected) {
            QCOMPARE(added.first(), QVariantList({messageId, QString("heart"), fixture.controller.ownerKey(), streamId, topicId}));
            QCOMPARE(adds, 1); QCOMPARE(removes, 0);
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", snapshot())));
            QCOMPARE(added.size(), 1);
            fixture.controller.toggleReaction(messageId, "heart");
            QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
            QCOMPARE(removes, 1); QCOMPARE(added.size(), 1);
        }
        if (operation == "logout-on-notify") QVERIFY(!fixture.controller.authenticated());
        if (operation == "route-on-notify") QCOMPARE(fixture.controller.selectedTopic(), secondTopicId);
        if (operation == "remove" || operation == "already-owned") { QCOMPARE(adds, 0); QCOMPARE(removes, 1); }
        if (operation.endsWith("replay")) QCOMPARE(adds, 1);
    }

    void realtimeReadRetainsStaleRawHistoryButDeletionCannotResurrectIt_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"message.read", "messages.read", "stream.read", "topic.read", "message.deleted"})
            QTest::newRow(operation) << QString::fromLatin1(operation);
    }
    void realtimeReadRetainsStaleRawHistoryButDeletionCannotResurrectIt()
    {
        QFETCH(QString, operation);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto first = message("raw content retained independently of event payload"); first["read"] = false;
        auto second = message("saved off-route history", returnedId, secondTopicId); second["read"] = false;
        fixture.messages = [&](const Request &request) {
            return Response{QJsonArray{QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId ? first : second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        const NativeCache::Key history{NativeCache::Kind::History, streamId, topicId, {}};
        const NativeCache::Key other{NativeCache::Kind::History, streamId, secondTopicId, {}};
        const auto before = cacheEntry(cache, cache.lease(), history); QVERIFY(before && before->fresh);
        QJsonObject payload;
        if (operation == "message.read") {
            payload = first; payload["read"] = true;
            payload["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "event content must not replace saved raw content"}};
        } else if (operation == "messages.read") {
            payload = QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{messageId, returnedId}}};
        } else if (operation == "stream.read") {
            payload = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false}};
        } else if (operation == "topic.read") {
            payload = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}};
        } else payload = QJsonObject{{"uuid", messageId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent(operation, payload)));
        const auto saved = cacheEntry(cache, cache.lease(), history);
        if (operation == "message.deleted") { QVERIFY(!saved); QVERIFY(cacheEntry(cache, cache.lease(), other)); return; }
        QVERIFY(saved && !saved->fresh); QCOMPARE(saved->savedAt, before->savedAt); QCOMPARE(saved->nextMarker, before->nextMarker);
        const bool confirmed = operation == "message.read" || operation == "messages.read";
        auto expected = first; expected["read"] = confirmed; QCOMPARE(saved->rows, QJsonArray{expected});
        const auto otherSaved = cacheEntry(cache, cache.lease(), other); QVERIFY(otherSaved);
        QCOMPARE(otherSaved->rows.first().toObject().value("read").toBool(), operation == "messages.read");
        QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}));
        QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, streamId, {}, {}}));
    }

    void confirmedViewportReadSurvivesRestartWithOfflineMessenger()
    {
        QTemporaryDir directory;
        Fixture fixture;
        bool offline = false; int history = 0, reads = 0, counterGets = 0;
        auto row = message("last known raw history"); row["read"] = false;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                ++reads; row["read"] = true; return Response{row};
            }
            ++history; return offline ? Response{QJsonObject{}, 503} : Response{QJsonArray{row}};
        };
        fixture.counters = [&](const Request &request) {
            ++counterGets;
            return Response{request.target.path().contains("/streams/")
                ? QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false},
                    {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", messageId}}
                : QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"},
                    {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", messageId}}};
        };
        {
            NativeCacheService cache(directory.path(), {}, true);
            NativeController current(&fixture.api, &fixture.session); current.useCache(&cache);
            current.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId);
            QTRY_COMPARE(current.streams().size(), 1); current.selectStream(streamId);
            QTRY_COMPARE(current.topics().size(), 1); current.selectTopic(topicId); QTRY_VERIFY(!current.busy());
            QCOMPARE(history, 1); current.markMessagesReadUpTo(messageId);
            QTRY_VERIFY(modelMessage(current, messageId).value("read").toBool());
            QTRY_COMPARE(counterGets, 2); QCOMPARE(reads, 1);
            QCOMPARE(history, 1); // A read acknowledgement must not add a history GET.
        }
        offline = true;
        fixture.streams = [&](const Request &) { return Response{QJsonObject{}, 503}; };
        fixture.topics = [&](const Request &) { return Response{QJsonObject{}, 503}; };
        fixture.users = [&](const Request &) { return Response{QJsonObject{}, 503}; };
        NativeCacheService reopened(directory.path(), {}, true);
        ApiClient api(nullptr, true); Session session(&api); NativeController restored(&api, &session); restored.useCache(&reopened);
        restored.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(restored.projects().size(), 1); restored.openProject(projectId);
        QTRY_COMPARE(restored.streams().size(), 1); restored.selectStream(streamId);
        QTRY_COMPARE(restored.topics().size(), 1); restored.selectTopic(topicId);
        QTRY_COMPARE(restored.messages().size(), 1);
        QCOMPARE(modelMessage(restored, messageId).value("content").toString(), "last known raw history");
        QVERIFY(modelMessage(restored, messageId).value("read").toBool());
        QVERIFY(restored.cacheStale()); QVERIFY(!restored.canSend()); QVERIFY(!restored.canLoadOlder());
        QVERIFY(!modelMessage(restored, messageId).value("canReact").toBool());
        QTRY_VERIFY(!restored.busy()); QCOMPARE(reads, 1); QCOMPARE(history, 2);
    }

    void verifiedTokenRotationReopensColdLeaseAndPreservesDraft()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        bool rotate = false, delay = false; int history = 0;
        fixture.authentication = [&](const Request &request) {
            if (rotate && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-rotated-cache-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.messages = [&](const Request &) {
            ++history; return Response{QJsonArray{message(rotate ? "verified renewal" : "old owner snapshot")}, 200, delay && !rotate ? 250 : 0};
        };
        QVERIFY(fixture.start()); const auto originalLease = cache.lease();
        fixture.controller.setDraft("preserve through renewal"); delay = true; fixture.controller.refresh(); QTRY_COMPARE(history, 2);
        QSignalSpy authenticated(&fixture.session, &Session::authenticated);
        rotate = true; fixture.session.requestRefresh(); QTRY_COMPARE(authenticated.size(), 1);
        QTRY_VERIFY(!fixture.controller.busy()); QTRY_COMPARE(fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(cache.lease().owner, originalLease.owner); QVERIFY(cache.lease().generation != originalLease.generation);
        QVERIFY(!cacheEntry(cache, originalLease, {NativeCache::Kind::History, streamId, topicId, {}}));
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "verified renewal");
        QCOMPARE(fixture.controller.draft(), "preserve through renewal"); QVERIFY(fixture.controller.canSend()); QCOMPARE(history, 3);
        const auto entry = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(entry); QVERIFY(entry->fresh);
    }

    void websocketReconnectStalesSavedPagesAndBlocksOfflineMutations()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        RealtimeGateway gateway(fixture.server.serverPort()); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            auto row = message("saved", topic == topicId ? messageId : returnedId, topic); row["read"] = false;
            return Response{QJsonArray{row}};
        };
        fixture.realtime = [&](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session); fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        QCOMPARE(history, 2); // Initial route + authoritative realtime baseline.
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        fixture.controller.setDraft("must wait for reconnect"); QVERIFY(fixture.controller.canSend());
        const auto writes = [&] { return std::count_if(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &request) {
            return request.method != "GET" && request.target.path().contains("/messenger/");
        }); };
        const auto reactionQueries = [&] { return std::count_if(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &request) {
            return request.target.path().contains("/message_reactions/");
        }); };
        const auto beforeWrites = writes(), beforeReactions = reactionQueries();
        gateway.sockets.first()->close(); QTRY_COMPARE(fixture.controller.syncState(), "retrying");
        QVERIFY(fixture.controller.cacheStale()); QVERIFY(!fixture.controller.canSend());
        const auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(saved); QVERIFY(!saved->fresh);
        fixture.controller.send(); fixture.controller.starMessage(messageId); fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.toggleReaction(messageId, "heart");
        QTRY_COMPARE(fixture.controller.syncState(), "live");
        QVERIFY(fixture.controller.canSend()); QCOMPARE(writes(), beforeWrites); QCOMPARE(reactionQueries(), beforeReactions);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 4);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 5);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 5);
    }

    void cacheOwnerSwitchFencesOldRepliesAndLogoutClearsOnlyThatOwner()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture first, second; first.controller.useCache(&cache); second.controller.useCache(&cache);
        bool delay = false; int delayed = 0;
        first.messages = [&](const Request &) { if (delay) ++delayed; return Response{QJsonArray{message(delay ? "late private A" : "private A")}, 200, delay ? 250 : 0}; };
        second.messages = [&](const Request &) { return Response{QJsonArray{message("private B")}}; };
        QVERIFY(first.start()); delay = true; first.controller.refresh(); QTRY_COMPARE(delayed, 1);
        QVERIFY(second.start());
        QCOMPARE(modelMessage(second.controller, messageId).value("content").toString(), "private B");
        QTRY_VERIFY(first.api.findChildren<QNetworkReply *>().isEmpty());
        const auto activeLease = cache.lease();
        auto active = cacheEntry(cache, activeLease, {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(active); QCOMPARE(active->rows.first().toObject().value("payload").toObject().value("content").toString(), "private B");
        first.controller.logout();
        QVERIFY(cache.available()); QCOMPARE(cache.lease().owner, activeLease.owner); QCOMPARE(cache.lease().generation, activeLease.generation);
        active = cacheEntry(cache, activeLease, {NativeCache::Kind::History, streamId, topicId, {}}); QVERIFY(active); QVERIFY(active->fresh);
        QVERIFY(flushCache(cache));
        NativeCache inspector(directory.path(), {}, true);
        QVERIFY(inspector.open(first.api.server(), projectId, userId));
        QVERIFY(!inspector.get(inspector.lease(), {NativeCache::Kind::History, streamId, topicId, {}}));
    }

    void unknownReactionLocationInvalidatesSavedHistorySafely()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("saved", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.created", QJsonObject{{"uuid", topicId}, {"project_id", projectId},
            {"message_uuid", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}, {"user_uuid", userId}, {"emoji_name", "heart"}, {"source_name", "native"}})));
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 4);
    }

    void expiredCacheHydratesButNetworkFailureCannotEnableSending()
    {
        QTemporaryDir directory; qint64 now = 1000;
        NativeCacheService cache(directory.path(), {}, true, [&] { return now; });
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        int history = 0; bool fail = false;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            if (fail) return Response{QJsonObject{}, 503, 100};
            return Response{QJsonArray{message(topic, topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        now += 120001; fail = true;
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(), secondTopicId);
        fixture.controller.setDraft("wait for verification"); QVERIFY(fixture.controller.cacheStale()); QVERIFY(!fixture.controller.canSend());
        QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3); QVERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.canSend()); QVERIFY(!modelMessage(fixture.controller, returnedId).value("canReact").toBool());
        fail = false; fixture.controller.refresh(); QTRY_VERIFY(!fixture.controller.busy()); QTRY_VERIFY(fixture.controller.canSend());
        QVERIFY(!fixture.controller.cacheStale()); QCOMPARE(history, 4);
    }

    void cachedOlderPagesAreReusedOnlyWithinTheVerifiedLiveProjection()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        QMap<int, QJsonObject> rows; for (int i = 1; i <= 70; ++i) rows.insert(i, historyMessage(i));
        int history = 0, markers = 0;
        fixture.messages = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId) return Response{QJsonArray{}};
            ++history; if (!QUrlQuery(request.target).queryItemValue("page_marker").isEmpty()) ++markers;
            return historyResponse(rows, request);
        };
        QVERIFY(fixture.start()); fixture.controller.loadOlder(); QTRY_COMPARE(fixture.controller.messages().size(), 70);
        QCOMPARE(history, 2); QCOMPARE(markers, 1);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(fixture.controller.messages().size(), 50);
        fixture.controller.loadOlder(); QTRY_COMPARE(fixture.controller.messages().size(), 70); QCOMPARE(history, 2); QCOMPARE(markers, 1);
        // A full baseline makes every cached marker stale. Deleting the old
        // first-page boundary must not send that saved cursor during hydration.
        rows.remove(21); fixture.controller.refresh(); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(history >= 4); QCOMPARE(markers, 2); QCOMPARE(fixture.controller.messages().size(), 69);
    }

    void cacheDestructionAndReplacementFenceDelayedWrites()
    {
        QTemporaryDir firstDirectory, secondDirectory;
        auto *firstCache = new NativeCacheService(firstDirectory.path(), {}, true);
        NativeCacheService replacement(secondDirectory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(firstCache);
        auto row = message("old"); int delayed = 0;
        fixture.messages = [&](const Request &) {
            if (row.value("payload").toObject().value("content") == "old delayed") { ++delayed; return Response{QJsonArray{row}, 200, 250}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start()); row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "old delayed"}};
        fixture.controller.refresh(); QTRY_COMPARE(delayed, 1);
        delete firstCache;
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "new service authoritative"}};
        fixture.controller.useCache(&replacement); QTRY_VERIFY(!fixture.controller.busy());
        // QNetworkAccessManager can retain an idle server-side connection.
        // Wait for the old HTTP operation, then check its model/cache effects.
        QTRY_COMPARE(fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "new service authoritative");
        const auto entry = cacheEntry(replacement, replacement.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(entry); QCOMPARE(entry->rows.first().toObject().value("payload").toObject().value("content").toString(), "new service authoritative");
        QVERIFY(entry->fresh);
    }

    void unavailableCacheFallsBackToAuthoritativeRest()
    {
        QTemporaryDir directory; QFile blocker(directory.filePath("not-a-directory")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.write("fixture"); blocker.close();
        NativeCacheService cache(blocker.fileName(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("network", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QVERIFY(!cache.available()); fixture.controller.setDraft("works without storage"); QVERIFY(fixture.controller.canSend());
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        fixture.controller.setDraft("still verified"); QVERIFY(fixture.controller.canSend()); QVERIFY(!fixture.controller.cacheStale());
    }

    void logoutClearsCurrentCacheBeforeOwnerIdentityDisappears()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [&](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache)); QVERIFY(cache.stats().entries >= 4);
        fixture.controller.logout(); QVERIFY(!cache.available()); QVERIFY(fixture.controller.messages().isEmpty());
        QVERIFY(cache.open(fixture.api.server(), projectId, userId));
        QVERIFY(!cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}}));
        QCOMPARE(cache.stats().entries, 0);
    }

    void cacheSnapshotTrustChangeNotifiesExistingMessageRows()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [&](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start());
        bool lastNotifiedCanReact = true;
        connect(&fixture.controller, &NativeController::messagesChanged, &fixture.controller, [&] {
            const auto row = modelMessage(fixture.controller, messageId);
            if (!row.isEmpty()) lastNotifiedCanReact = row.value("canReact").toBool();
        });
        fixture.controller.reloadSnapshot(); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(modelMessage(fixture.controller, messageId).value("canReact").toBool());
        QVERIFY(lastNotifiedCanReact);
    }

    void cacheMutationInvalidatesOnlyTheAffectedConversation_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"star", "edit", "delete", "read", "reaction", "send"}) QTest::newRow(operation) << QString::fromLatin1(operation);
    }
    void cacheMutationInvalidatesOnlyTheAffectedConversation()
    {
        QFETCH(QString, operation);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto first = message("before"); first["read"] = false; first["reactions"] = QJsonObject{}; first["reaction_users"] = QJsonObject{};
        QMap<QString, QJsonObject> rows{{messageId, first}, {returnedId, message("other route", returnedId, secondTopicId)}};
        int history = 0; bool reacted = false;
        const QJsonObject fact{{"uuid", topicId}, {"project_id", projectId}, {"message_uuid", messageId}, {"user_uuid", userId},
            {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/star/invoke")) { rows[messageId]["starred"] = true; return Response{rows.value(messageId)}; }
            if (path.endsWith("/actions/read_up_to/invoke")) { rows[messageId]["read"] = true; return Response{rows.value(messageId)}; }
            if (request.method == "PUT") { rows[messageId]["payload"] = request.body.value("payload"); return Response{rows.value(messageId)}; }
            if (request.method == "DELETE") { rows.remove(messageId); return Response{QJsonObject{}, 204}; }
            if (request.method == "POST") {
                const auto id = request.body.value("uuid").toString();
                rows[id] = message(request.body.value("payload").toObject().value("content").toString(), id);
                return Response{rows.value(id)};
            }
            if (!path.endsWith('/')) return Response{rows.value(path.section('/', -1))};
            ++history; QJsonArray result; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            for (const auto &row : rows) if (row.value("topic_uuid").toString() == topic) result.append(row);
            return Response{result};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                reacted = true; rows[messageId]["reactions"] = QJsonObject{{"heart", 1}}; rows[messageId]["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
                return Response{fact, 201};
            }
            return Response{reacted ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        if (operation == "star") fixture.controller.starMessage(messageId);
        if (operation == "edit") fixture.controller.editMessage(messageId, "edited");
        if (operation == "delete") fixture.controller.deleteMessage(messageId);
        if (operation == "read") fixture.controller.markMessagesReadUpTo(messageId);
        if (operation == "reaction") fixture.controller.toggleReaction(messageId, "heart");
        if (operation == "send") { fixture.controller.setDraft("sent"); fixture.controller.send(); }
        const auto changed = [&] {
            const auto row = modelMessage(fixture.controller, messageId);
            if (operation == "delete") return row.isEmpty();
            if (operation == "star") return row.value("starred").toBool();
            if (operation == "edit") return row.value("content").toString() == "edited";
            if (operation == "read") return row.value("read").toBool();
            if (operation == "reaction") return row.value("reactions").toList().size() == 1 && !row.value("reactionBusy").toBool();
            return fixture.controller.draft().isEmpty() && fixture.controller.messages().size() == 2;
        };
        QTRY_VERIFY(changed());
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(history, 3); QVERIFY(changed());
    }

    void offRouteRealtimeInvalidatesHistoryWithoutEvictingOtherRoutes()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto first = message("before"); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{topic == topicId ? first : message("other", returnedId, secondTopicId)}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        first["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "live off-route edit"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", first)));
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "live off-route edit");
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
    }

    void deletedPeerMessageLeavesTheOtherRowsWhenReopeningOffline()
    {
        QTemporaryDir directory;
        NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture;
        fixture.extraTopic = true;
        fixture.controller.useCache(&cache);
        auto first = historyMessage(1);
        auto removed = historyMessage(2);
        auto last = historyMessage(3);
        removed["author_uuid"] = secondTopicId;
        removed["is_own"] = false;
        const auto deletedId = removed.value("uuid").toString();
        bool offline = false;
        int offlineRequests = 0;
        fixture.messages = [&](const Request &request) {
            if (offline) {
                ++offlineRequests;
                return Response{QJsonObject{}, 503};
            }
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{message("Second topic", returnedId, secondTopicId)}};
            return Response{QJsonArray{last, removed, first}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.messages().size(), 3);
        QVERIFY(flushCache(cache));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", {
            {"uuid", deletedId}, {"stream_uuid", streamId}, {"topic_uuid", topicId},
            {"author_uuid", secondTopicId}})));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(flushCache(cache));
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!modelMessage(fixture.controller, returnedId).isEmpty());
        offline = true;
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(offlineRequests > 0 && !fixture.controller.busy());
        QCOMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(!modelMessage(fixture.controller, first.value("uuid").toString()).isEmpty());
        QVERIFY(!modelMessage(fixture.controller, last.value("uuid").toString()).isEmpty());
        QVERIFY(modelMessage(fixture.controller, deletedId).isEmpty());
        QVERIFY(!fixture.controller.messageHistoryReady());
    }

    void warmCacheNavigationAvoidsRedundantRestRequests()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        int history = 0, topics = 0;
        fixture.topics = [&](const Request &) { ++topics; return Response{QJsonArray{
            QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "One"}},
            QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Two"}}}}; };
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message(topic, topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QCOMPARE(history, 1); QCOMPARE(topics, 1);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), topicId);
        QCOMPARE(history, 2);
        fixture.controller.selectStream({}); fixture.controller.selectStream(streamId);
        QTRY_COMPARE(fixture.controller.topics().size(), 2); QCOMPARE(topics, 1);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        QVERIFY(cache.stats().hits >= 3);
    }

    void coldDiskHydrationIsVisibleButCannotAuthorizeMutations()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        auto row = message("saved offline text"); row["read"] = false;
        fixture.messages = [&](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        // Seed a previous process' validated snapshots independently of the
        // warm-navigation test, so this exercises cold hydration itself.
        QVERIFY(cache.open(fixture.api.server(), projectId, userId));
        const auto lease = cache.lease();
        QVERIFY(cache.put(lease, {NativeCache::Kind::Streams, {}, {}, {}}, QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Previously plaintext"}, {"encryption", false}}}));
        QVERIFY(cache.put(lease, {NativeCache::Kind::Users, {}, {}, {}}, QJsonArray{QJsonObject{{"uuid", userId}, {"username", "fallback"}, {"name", "Saved preferred name"}}}));
        QVERIFY(cache.put(lease, {NativeCache::Kind::Topics, streamId, {}, {}}, QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Saved topic"}}}));
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, streamId, topicId, {}}, QJsonArray{row}));
        QVERIFY(flushCache(cache));
        cache.close();
        QVERIFY(flushCache(cache));
        NativeCacheService reopened(directory.path(), {}, true);
        NativeController restored(&fixture.api, &fixture.session);
        fixture.encrypted = true;
        fixture.users = [&](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", "fallback"}, {"name", "Verified preferred name"}}}, 200, 350}; };
        fixture.streams = [&](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
            {"name", "Now encrypted"}, {"encryption", true}}}, 200, 350}; };
        fixture.topics = [&](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Current"}}}, 200, 350}; };
        fixture.messages = [&](const Request &) {
            auto current = row; current["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId},
                {"content", QString::fromLatin1(QByteArray("opaque-never-render").toBase64())}};
            return Response{QJsonArray{current}, 200, 600};
        };
        restored.useCache(&reopened);
        QTRY_COMPARE(restored.streams().size(), 1);
        restored.selectStream(streamId); QTRY_COMPARE(restored.topics().size(), 1);
        restored.selectTopic(topicId);
        QTRY_COMPARE(restored.messages().size(), 1);
        QCOMPARE(modelMessage(restored, messageId).value("content").toString(), "saved offline text");
        QCOMPARE(modelMessage(restored, messageId).value("author").toString(), "Saved preferred name");
        QVERIFY(restored.cacheStale()); QVERIFY(!restored.canLoadOlder());
        restored.setDraft("must not send from stale encryption metadata");
        QVERIFY(!restored.canSend()); QVERIFY(!modelMessage(restored, messageId).value("canReact").toBool());
        const auto before = fixture.server.requests.size();
        restored.send(); restored.editMessage(messageId, "bad"); restored.starMessage(messageId); restored.deleteMessage(messageId);
        restored.markMessagesReadUpTo(messageId); restored.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!restored.busy());
        for (qsizetype i = before; i < fixture.server.requests.size(); ++i) QVERIFY(fixture.server.requests.at(i).method == "GET");
        QVERIFY(!modelMessage(restored, messageId).value("content").toString().contains("opaque-never-render"));
        QVERIFY(!modelMessage(restored, messageId).value("content").toString().contains(QString::fromLatin1(QByteArray("opaque-never-render").toBase64())));
        QCOMPARE(modelMessage(restored, messageId).value("kind").toString(), "e2ee");
        QCOMPARE(modelMessage(restored, messageId).value("author").toString(), "Verified preferred name");
        QVERIFY(!restored.canSend());
    }

    void bottomReadAnchorRepairsAlreadyReadTail_data()
    {
        QTest::addColumn<bool>("staleCounter");
        QTest::newRow("counter-clears") << false;
        QTest::newRow("stale-counter-deduplicates") << true;
    }
    void bottomReadAnchorRepairsAlreadyReadTail()
    {
        QFETCH(bool, staleCounter);
        Fixture fixture;
        auto row = message(); row["read"] = true;
        auto topic = creationTopic(topicId, streamId, "Read anchor");
        topic["unread_count"] = topic["active_unread_count"] = 3;
        topic["last_message_uuid"] = messageId;
        auto stream = QJsonObject{{"uuid", streamId}, {"project_id", projectId},
            {"name", "Read anchor"}, {"encryption", false}, {"unread_count", 3},
            {"active_unread_count", 3}, {"passive_unread_count", 0}, {"last_message_uuid", messageId}};
        int posts = 0, counterGets = 0; QString postedPath;
        fixture.topics = [&](const Request &) { return Response{QJsonArray{topic}}; };
        fixture.streams = [&](const Request &) { return Response{QJsonArray{stream}}; };
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                ++posts; postedPath = request.target.path();
                if (!staleCounter) {
                    topic["unread_count"] = topic["active_unread_count"] = 0;
                    stream["unread_count"] = stream["active_unread_count"] = 0;
                }
                return Response{row, 200, 100, {}};
            }
            return Response{QJsonArray{row}};
        };
        fixture.counters = [&](const Request &request) {
            ++counterGets;
            return Response{request.target.path().contains("/stream_topics/") ? topic : stream};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTest::qWait(100); QCOMPARE(posts, 0); // ordinary admission still requires unread rows
        fixture.controller.markTopicReadAtBottom();
        fixture.controller.markTopicReadAtBottom(); // in-flight deduplication
        QTRY_COMPARE(posts, 1);
        QCOMPARE(postedPath, QString("/api/workspace/v1/messenger/messages/") + messageId + "/actions/read_up_to/invoke");
        QTRY_COMPARE(counterGets, 2);
        QTRY_COMPARE(fixture.controller.topics().first().toMap().value("unread").toInt(), staleCounter ? 3 : 0);
        QTRY_COMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), staleCounter ? 3 : 0);
        for (int i = 0; i < 3; ++i) fixture.controller.markTopicReadAtBottom();
        QTest::qWait(200); QCOMPARE(posts, 1); QCOMPARE(counterGets, 2);
    }

    void bottomReadAnchorRequiresCurrentLoadedTopic_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"zero-counter", "unknown-last", "no-last", "aggregate", "logout"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void bottomReadAnchorRequiresCurrentLoadedTopic()
    {
        QFETCH(QString, scenario);
        Fixture fixture;
        auto row = message(); row["read"] = true;
        auto topic = creationTopic(topicId, streamId, "Read anchor");
        topic["unread_count"] = topic["active_unread_count"] = scenario == "zero-counter" ? 0 : 3;
        topic["last_message_uuid"] = scenario == "no-last" ? QJsonValue::Null
            : QJsonValue(scenario == "unknown-last" ? returnedId : messageId);
        int posts = 0;
        fixture.topics = [&](const Request &) { return Response{QJsonArray{topic}}; };
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { ++posts; return Response{row}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        if (scenario == "aggregate") fixture.controller.selectStream(streamId);
        if (scenario == "logout") fixture.session.logout();
        QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.markTopicReadAtBottom();
        QTest::qWait(200); QCOMPARE(posts, 0);
    }

    void incompleteReadCounterSnapshotsCannotInventZeroUnread()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        const auto streamSnapshot = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false},
            {"unread_count", 7}, {"active_unread_count", 5}, {"passive_unread_count", 2}, {"last_message_uuid", returnedId}};
        const auto topicSnapshot = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"},
            {"unread_count", 5}, {"active_unread_count", 3}, {"passive_unread_count", 2}, {"last_message_uuid", returnedId}};
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { auto response = row; response["read"] = true; return Response{response}; }
            return Response{QJsonArray{row}};
        };
        fixture.counters = [&](const Request &request) {
            auto response = request.target.path().contains("/streams/") ? streamSnapshot : topicSnapshot;
            response.remove("unread_count");
            return Response{response};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", streamSnapshot)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topicSnapshot, 13)));
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), 7);
        QCOMPARE(fixture.controller.topics().first().toMap().value("unread").toInt(), 5);
    }

    void readMarkerOwnerGenerationAndMalformedAcknowledgements_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"generation", "wrong-project", "wrong-id", "wrong-topic", "wrong-owner", "read-string", "changed-time"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void readMarkerOwnerGenerationAndMalformedAcknowledgements()
    {
        QFETCH(QString, scenario);
        Fixture fixture;
        auto row = message(); row["read"] = false;
        int reads = 0, counterGets = 0;
        fixture.counters = [&](const Request &) { ++counterGets; return Response{QJsonObject{}}; };
        fixture.messages = [&](const Request &request) {
            if (!request.target.path().endsWith("/actions/read_up_to/invoke")) return Response{QJsonArray{row}};
            ++reads;
            auto response = row; response["read"] = true;
            if (scenario == "wrong-project") response["project_id"] = streamId;
            if (scenario == "wrong-id") response["uuid"] = returnedId;
            if (scenario == "wrong-topic") response["topic_uuid"] = secondTopicId;
            if (scenario == "wrong-owner") response["user_uuid"] = streamId;
            if (scenario == "read-string") response["read"] = "true";
            if (scenario == "changed-time") response["created_at"] = "2026-01-02T10:00:01Z";
            return Response{response, 200, scenario == "generation" ? 100 : 0, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE_WITH_TIMEOUT(reads, 1, 2000);
        if (scenario == "generation") fixture.api.setAccessToken("replacement-token");
        else QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.error().isEmpty(), 2000);
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
        QCOMPARE(counterGets, 0);
        QCOMPARE(reads, 1);
    }

    void admittedReadUsesBoundedExactGetAndCoalescesNewerViewportAnchor()
    {
        Fixture fixture;
        auto one = message("first", messageId), two = message("second", returnedId);
        one["read"] = two["read"] = false;
        two["created_at"] = "2026-01-02T10:00:01Z";
        int posts = 0, gets = 0;
        fixture.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/read_up_to/invoke")) { ++posts; return Response{path.contains(returnedId) ? two : one}; }
            if (request.method == "GET" && (path.endsWith(messageId) || path.endsWith(returnedId))) {
                ++gets;
                auto row = path.endsWith(returnedId) ? two : one;
                row["read"] = true;
                return Response{row, 200, 100, {}};
            }
            return Response{QJsonArray{two, one}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.markMessagesReadUpTo(returnedId);
        for (int i = 0; i < 100; ++i) fixture.controller.markMessagesReadUpTo(returnedId);
        QTRY_VERIFY_WITH_TIMEOUT(modelMessage(fixture.controller, returnedId).value("read").toBool(), 2000);
        QCOMPARE(posts, 2); QCOMPARE(gets, 2);
        QVERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
    }

    void unappliedReadAdmissionIsBoundedWithoutRepeatingMutation()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        int posts = 0, gets = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++posts; return Response{row}; }
            if (request.target.path().endsWith(messageId)) { ++gets; return Response{row}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.error().isEmpty(), 2500);
        QCOMPARE(posts, 1); QCOMPARE(gets, 3);
        QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
        for (int i = 0; i < 100; ++i) fixture.controller.markMessagesReadUpTo(messageId);
        QCOMPARE(posts, 1); QCOMPARE(gets, 3);
    }

    void readCountersAreAuthoritativeAndCannotOverwriteLaterRealtimeSnapshots()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        auto streamSnapshot = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false},
            {"unread_count", 4}, {"active_unread_count", 3}, {"passive_unread_count", 1}, {"last_message_uuid", returnedId}};
        auto topicSnapshot = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"},
            {"unread_count", 2}, {"active_unread_count", 1}, {"passive_unread_count", 1}, {"last_message_uuid", returnedId}};
        int counterGets = 0;
        fixture.counters = [&](const Request &request) { ++counterGets; return Response{request.target.path().contains("/streams/") ? streamSnapshot : topicSnapshot}; };
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { auto response = row; response["read"] = true; return Response{response}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE(counterGets, 2);
        QTRY_COMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), 4);
        QTRY_COMPARE(fixture.controller.topics().first().toMap().value("unread").toInt(), 2);
        QCOMPARE(fixture.controller.streams().first().toMap().value("activeUnread").toInt(), 3);
        QCOMPARE(fixture.controller.topics().first().toMap().value("lastMessage").toString(), returnedId);
        fixture.counters = [&](const Request &request) { ++counterGets; return Response{request.target.path().contains("/streams/") ? streamSnapshot : topicSnapshot, 200, 150, {}}; };
        // A fresh unread arrival enables another viewport mutation with delayed counter GETs.
        auto incoming = historyMessage(700); incoming["read"] = false;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", incoming)));
        fixture.messages = [&](const Request &request) {
            auto response = incoming; response["read"] = true;
            return Response{request.target.path().endsWith("/actions/read_up_to/invoke") ? QJsonValue(response) : QJsonValue(QJsonArray{row, incoming})};
        };
        fixture.controller.markMessagesReadUpTo(incoming.value("uuid").toString());
        QTRY_COMPARE(counterGets, 4);
        streamSnapshot["unread_count"] = 7; topicSnapshot["unread_count"] = 5;
        streamSnapshot["last_message_uuid"] = incoming.value("uuid"); topicSnapshot["last_message_uuid"] = incoming.value("uuid");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", streamSnapshot, 20)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topicSnapshot, 21)));
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), 7);
        QCOMPARE(fixture.controller.topics().first().toMap().value("unread").toInt(), 5);
        QCOMPARE(fixture.controller.streams().first().toMap().value("lastMessage").toString(), incoming.value("uuid").toString());
    }

    void viewportReadMarkerUpdatesOnlyFlagsThroughExactBoundary()
    {
        Fixture fixture;
        auto earlier = message("earlier", returnedId);
        auto anchor = message("old anchor", messageId);
        auto later = historyMessage(99);
        earlier["created_at"] = "2026-01-02T10:00:00.000100Z";
        anchor["created_at"] = "2026-01-02T10:00:00.000200Z";
        later["created_at"] = "2026-01-02T10:00:00.000300Z";
        earlier["read"] = anchor["read"] = later["read"] = false;
        int reads = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                ++reads;
                auto response = anchor; response["read"] = true;
                return Response{response, 200, 100, {}};
            }
            return Response{QJsonArray{later, anchor, earlier}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(reads, 0); // Loading a conversation never marks it as read.
        fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE(reads, 1);
        auto updated = anchor; updated["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "live edit"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated)));
        auto arriving = historyMessage(200); arriving["created_at"] = "2026-01-02T10:00:00.000150Z"; arriving["read"] = false;
        const auto arrivingId = arriving.value("uuid").toString();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arriving, 13)));
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "live edit");
        QVERIFY(modelMessage(fixture.controller, returnedId).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, later.value("uuid").toString()).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, arrivingId).value("read").toBool());
        // An older queued event cannot undo a locally confirmed monotonic read flag.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 14)));
        QVERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        fixture.controller.markMessagesReadUpTo(messageId);
        QCOMPARE(reads, 1);
    }

    void failedReadMarkerIsBoundedAndDoesNotChangeFlags()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        int reads = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE(reads, 3);
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.messages().first().toMap().value("read").toBool());
        for (int i = 0; i < 200; ++i) fixture.controller.markMessagesReadUpTo(messageId);
        QCOMPARE(reads, 3);
    }

    void readMarkerRejectsStaleRouteAndWrongOwnerResponses()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        auto row = message(); row["read"] = false;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { auto response = row; response["read"] = true; return Response{response, 200, 100, {}}; }
            auto response = row;
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId) response["topic_uuid"] = secondTopicId;
            return Response{QJsonArray{response}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        // New route contains the same synthetic ID to expose a missing route fence.
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QVERIFY(!fixture.controller.messages().first().toMap().value("read").toBool());
        fixture.messages = [&](const Request &request) {
            auto response = row; response["topic_uuid"] = secondTopicId;
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { response["read"] = true; response["user_uuid"] = streamId; return Response{response}; }
            return Response{QJsonArray{response}};
        };
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.messages().first().toMap().value("read").toBool());
    }

    void snapshotBarrierWaitsForSelectedHistoryAndUsers()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}, 200, 250, {}}; };
        fixture.users = [](const Request &) { return Response{QJsonArray{}, 200, 100, {}}; };
        QVERIFY(fixture.start());
        QSignalSpy ready(&fixture.controller, &NativeController::snapshotReloaded);
        fixture.controller.reloadSnapshot();
        QVERIFY(fixture.controller.busy());
        QTest::qWait(150);
        QCOMPARE(ready.size(), 0);
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(!fixture.controller.busy());
    }

    void failedSnapshotCannotAcknowledgeRealtimeBaseline()
    {
        Fixture fixture;
        bool fail = false;
        fixture.messages = [&fail](const Request &) {
            return fail ? Response{QJsonObject{}, 403, 0, {}} : Response{QJsonArray{message()}};
        };
        QVERIFY(fixture.start());
        QSignalSpy ready(&fixture.controller, &NativeController::snapshotReloaded);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        fail = true;
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(ready.size(), 0);
    }

    void realtimeMessagesAreOwnerScopedAndFenceOldSnapshots()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start());
        auto body = message("Live message", returnedId);
        body.insert("kind", "message.created");
        QJsonObject event{{"uuid", secondTopicId}, {"project_id", projectId}, {"user_uuid", userId},
                          {"payload", body}};
        QVERIFY(fixture.controller.applyRealtimeEvent(event));
        QCOMPARE(fixture.controller.messages().size(), 2);
        event.insert("user_uuid", streamId);
        QVERIFY(!fixture.controller.applyRealtimeEvent(event));
        QCOMPARE(fixture.controller.messages().size(), 2);
        event.insert("user_uuid", userId);
        event.insert("payload", QJsonObject{{"kind", "message.deleted"}, {"uuid", returnedId},
                                            {"stream_uuid", streamId}, {"topic_uuid", topicId}});
        QVERIFY(fixture.controller.applyRealtimeEvent(event));
        QCOMPARE(fixture.controller.messages().size(), 1);
        event.insert("payload", QJsonObject{{"kind", "future.domain.event"}});
        QVERIFY(fixture.controller.applyRealtimeEvent(event));
        event.insert("payload", QJsonObject{{"kind", "message.created"}, {"uuid", "bad"}});
        QVERIFY(!fixture.controller.applyRealtimeEvent(event));
    }

    void refreshReconcilesRemoteDeletionAndNewTopics()
    {
        Fixture fixture;
        bool deleted = false;
        fixture.messages = [&deleted](const Request &) {
            return Response{deleted ? QJsonArray{message()} : QJsonArray{message(), message("Deleted elsewhere", returnedId)}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.setDraft("Keep this draft");
        deleted = true;
        fixture.extraTopic = true;
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QTRY_COMPARE(fixture.controller.topics().size(), 2);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QCOMPARE(fixture.controller.draft(), "Keep this draft");
    }

    void refreshBridgesMoreThanOnePageOfNewArrivals()
    {
        Fixture fixture;
        int total = 50;
        fixture.messages = [&total](const Request &request) {
            const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
            int highest = total;
            if (!marker.isEmpty())
                highest = marker.right(12).toInt(nullptr, 16) - 1;
            QJsonArray items;
            for (int n = highest; n > std::max(0, highest - 50); --n)
                items.append(historyMessage(n));
            Response response{items};
            if (highest > 50)
                response.headers.insert("X-Pagination-Marker", historyMessage(highest - 49).value("uuid").toString().toUtf8());
            return response;
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.messages().size(), 50);
        total = 110;
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().size(), 110);
        for (int n = 1; n <= 110; ++n)
            QCOMPARE(fixture.controller.messages().at(n - 1).toMap().value("id").toString(),
                     historyMessage(n).value("uuid").toString());
    }

    void verifiesMutationOutsideLatestPage()
    {
        Fixture fixture;
        auto old = historyMessage(1);
        fixture.messages = [&old](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/star/invoke")) {
                old.insert("starred", true);
                return Response{old};
            }
            if (path.endsWith("/messages/" + old.value("uuid").toString()))
                return Response{old};
            const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
            const int highest = marker.isEmpty() ? 100 : 50;
            QJsonArray items;
            for (int n = highest; n > highest - 50; --n)
                items.append(n == 1 ? old : historyMessage(n));
            Response response{items};
            if (marker.isEmpty())
                response.headers.insert("X-Pagination-Marker", historyMessage(51).value("uuid").toString().toUtf8());
            return response;
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.messages().size(), 50);
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 100);
        fixture.controller.starMessage(old.value("uuid").toString());
        QTRY_VERIFY(fixture.controller.messages().at(0).toMap().value("starred").toBool());
        QCOMPARE(fixture.controller.messages().size(), 100);
    }

    void refreshReconcilesAllPreviouslyLoadedRows()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        fixture.messages = [&rows](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 100);
        rows.remove(20);
        rows[21].insert("payload", QJsonObject{{"kind", "markdown"}, {"content", "Edited remotely"}});
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().size(), 99);
        QCOMPARE(fixture.controller.messages().at(19).toMap().value("content").toString(), "Edited remotely");
    }

    void refreshReplacesDeletedOlderCursor()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        fixture.messages = [&rows](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        rows.remove(51);
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().first().toMap().value("id").toString(),
                     historyMessage(50).value("uuid").toString());
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 99);
        QVERIFY(fixture.controller.error().isEmpty());
    }

    void deletedOlderCursorRecoversWithoutManualRefresh()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        fixture.messages = [&rows](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        rows.remove(51);
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 99);
        QVERIFY(fixture.controller.error().isEmpty());
    }

    void missingCursorRecoveryIsBounded()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        int olderRequests = 0;
        fixture.messages = [&rows, &olderRequests](const Request &request) {
            if (!QUrlQuery(request.target).queryItemValue("page_marker").isEmpty()) {
                ++olderRequests;
                return Response{QJsonObject{}, 404, 0, {}};
            }
            return historyResponse(rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.loadOlder();
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.busy());
        QCOMPARE(olderRequests, 2);
        QTest::qWait(100);
        QCOMPARE(olderRequests, 2);
        QCOMPARE(fixture.controller.messages().size(), 50);
    }

    void delayedHistoryCannotUndoVerifiedMutation_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"send", "edit", "star", "delete"})
            QTest::newRow(operation) << QString::fromLatin1(operation);
    }

    void delayedHistoryCannotUndoVerifiedMutation()
    {
        QFETCH(QString, operation);
        Fixture fixture;
        QJsonObject existing = message("Before");
        QJsonObject sent;
        bool deleted = false;
        bool delayNextSnapshot = false;
        int delayedSnapshots = 0;
        fixture.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (request.method == "POST" && path.endsWith("/messages/")) {
                sent = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{sent};
            }
            if (request.method == "PUT") {
                existing.insert("payload", request.body.value("payload"));
                return Response{existing};
            }
            if (request.method == "DELETE") {
                deleted = true;
                return Response{QJsonObject{}};
            }
            if (path.endsWith("/actions/star/invoke")) {
                existing.insert("starred", true);
                return Response{existing};
            }
            if (path.endsWith("/messages/")) {
                QJsonArray items;
                if (!deleted)
                    items.append(existing);
                if (!sent.isEmpty())
                    items.append(sent);
                Response response{items};
                if (delayNextSnapshot) {
                    response.delay = 350;
                    delayNextSnapshot = false;
                    ++delayedSnapshots;
                }
                return response;
            }
            return Response{path.endsWith(returnedId) ? sent : existing};
        };
        QVERIFY(fixture.start());
        delayNextSnapshot = true;
        fixture.controller.refresh();
        QTRY_COMPARE(delayedSnapshots, 1);
        const auto changed = [&] {
            const auto items = fixture.controller.messages();
            if (operation == "delete")
                return items.isEmpty();
            if (operation == "send")
                return items.size() == 2 && fixture.controller.draft().isEmpty();
            return !items.isEmpty() && (operation == "star"
                ? items.first().toMap().value("starred").toBool()
                : items.first().toMap().value("content").toString() == "After");
        };
        if (operation == "send") {
            fixture.controller.setDraft("Sent during an older snapshot");
            fixture.controller.send();
        } else if (operation == "edit") {
            fixture.controller.editMessage(messageId, "After");
        } else if (operation == "star") {
            fixture.controller.starMessage(messageId);
        } else {
            fixture.controller.deleteMessage(messageId);
        }
        QTRY_VERIFY(changed());
        QTest::qWait(450);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(changed());
    }

    void delayedUsersNotifyDisplayedMessageAuthors()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        fixture.users = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", "Resolved author"}}}, 200, 700, {}};
        };
        QVERIFY(fixture.start());
        bool authorNotified = false;
        connect(&fixture.controller, &NativeController::messagesChanged, &fixture.controller, [&] {
            const auto rows = fixture.controller.messages();
            if (!rows.isEmpty() && rows.first().toMap().value("author") == "Resolved author")
                authorNotified = true;
        });
        QTRY_VERIFY(authorNotified);
    }

    void liveTopicUpdateSurvivesDelayedTopicSnapshot()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        int requests = 0;
        fixture.topics = [&requests](const Request &) {
            const bool stale = ++requests == 1;
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", stale ? "Old topic" : "Live topic"}}}, 200, stale ? 250 : 0, {}};
        };
        fixture.controller.selectStream(streamId);
        QTRY_COMPARE(requests, 1);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Live topic"}})));
        QCOMPARE(fixture.controller.topics().first().toMap().value("name").toString(), "Live topic");
        QTest::qWait(350);
        QCOMPARE(fixture.controller.topics().first().toMap().value("name").toString(), "Live topic");
    }

    void snapshotBarrierJoinsUsersTopicsAndHistoryAndRejectsFailure()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}, 200, 400, {}}; };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Keep the draft through snapshot reload");
        fixture.users = [](const Request &) { return Response{QJsonArray{}, 200, 200, {}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}, 200, 300, {}};
        };
        QSignalSpy loaded(&fixture.controller, &NativeController::snapshotReloaded);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        fixture.controller.reloadSnapshot();
        QTest::qWait(350);
        QCOMPARE(loaded.size(), 0);
        QTRY_COMPARE(loaded.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(fixture.controller.draft(), "Keep the draft through snapshot reload");
        fixture.users = [](const Request &) { return Response{QJsonObject{}, 503, 0, {}}; };
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(failed.size(), 1);
        QTest::qWait(450);
        QCOMPARE(loaded.size(), 1);
    }

    void obsoleteSnapshotFailureCannotCancelNewSnapshotBarrier()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        int userRequests = 0;
        fixture.users = [&userRequests](const Request &) {
            if (++userRequests == 1)
                return Response{QJsonObject{}, 503, 250, {}};
            return Response{QJsonArray{}, 200, 400, {}};
        };
        QSignalSpy loaded(&fixture.controller, &NativeController::snapshotReloaded);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(userRequests, 1);
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(userRequests, 2);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 1500);
        QCOMPARE(failed.size(), 0);
    }

    void integratedRealtimeAppliesAndAcknowledgesLiveMessage_data()
    {
        QTest::addColumn<bool>("aggregate"); QTest::newRow("topic") << false; QTest::newRow("aggregate") << true;
    }
    void integratedRealtimeAppliesAndAcknowledgesLiveMessage()
    {
        QFETCH(bool, aggregate);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.aggregateMessages = aggregate; fixture.extraTopic = aggregate;
        fixture.controller.useCache(&cache);
        RealtimeGateway gateway(fixture.server.serverPort());
        QVector<QJsonObject> events;
        fixture.messages = [](const Request &request) {
            return Response{QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId ? QJsonArray{} : QJsonArray{message()}};
        };
        fixture.realtime = [&events](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows;
            const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : events)
                if (event.value("epoch_version").toInt() > after)
                    rows.append(event);
            return Response{rows};
        };
        QVERIFY(fixture.start(gateway.origin()));
        if (aggregate) {
            fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
            fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 1); QTRY_VERIFY(!fixture.controller.busy());
        }
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live");
        QTRY_COMPARE(gateway.sockets.size(), 1);
        const auto event = controllerEvent("message.created", message("Delivered over the native realtime path", returnedId, aggregate ? secondTopicId : topicId));
        events.append(event);
        gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(12));
        QCOMPARE(fixture.controller.messages().last().toMap().value("content").toString(), "Delivered over the native realtime path");
        if (aggregate) {
            const auto savedAggregate = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, {}, {}});
            const auto savedTopic = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, secondTopicId, {}});
            QVERIFY(savedAggregate && savedTopic); QVERIFY(!savedAggregate->fresh && !savedTopic->fresh);
            QCOMPARE(savedAggregate->rows.size(), 2); QCOMPARE(savedTopic->rows.size(), 1);
            QCOMPARE(savedTopic->rows.first().toObject().value("uuid").toString(), returnedId);
        }
    }

    void reconnectRevalidatesOnlyTheFailedSelectedHistory_data()
    {
        QTest::addColumn<bool>("leaveRoute");
        QTest::addColumn<bool>("forbidden");
        QTest::addColumn<bool>("lateFailure");
        QTest::newRow("offline-topic") << false << false << false;
        QTest::newRow("http-failure-after-reconnect") << false << false << true;
        QTest::newRow("left-topic") << true << false << false;
        QTest::newRow("permission-denied") << false << true << false;
    }

    void reconnectRevalidatesOnlyTheFailedSelectedHistory()
    {
        QFETCH(bool, leaveRoute);
        QFETCH(bool, forbidden);
        QFETCH(bool, lateFailure);
        Fixture fixture;
        fixture.extraTopic = true;
        RealtimeGateway gateway(fixture.server.serverPort());
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        bool online = false, sawHistoryError = false;
        bool holdFailure = lateFailure;
        std::function<void()> release;
        connect(&fixture.controller, &NativeController::stateChanged, this, [&] {
            if (fixture.controller.error().contains("Could not load messages")) {
                sawHistoryError = true;
                online = true;
            }
        });
        int revalidated = 0;
        fixture.messages = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") != secondTopicId)
                return Response{QJsonArray{}};
            if (!online) {
                if (forbidden) return Response{QJsonObject{}, 403};
                Response offline;
                offline.abortConnection = true;
                if (holdFailure)
                    offline.defer = [&](std::function<void()> deliver) { release = std::move(deliver); };
                return offline;
            }
            ++revalidated;
            auto row = message("History restored after reconnect");
            row["topic_uuid"] = secondTopicId;
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live");
        QVERIFY(!gateway.sockets.isEmpty() && gateway.sockets.last());
        gateway.sockets.last()->close();
        QTRY_COMPARE(fixture.controller.syncState(), "retrying");
        fixture.controller.selectTopic(secondTopicId);
        if (lateFailure) {
            QTRY_VERIFY(static_cast<bool>(release));
            QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.syncState(), "live", 5000);
            holdFailure = false;
            release();
        }
        QTRY_VERIFY(sawHistoryError);
        if (leaveRoute) fixture.controller.selectTopic(topicId);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.syncState(), "live", 5000);
        if (!leaveRoute && !forbidden) {
            QTRY_COMPARE_WITH_TIMEOUT(revalidated, 1, 2000);
            QTRY_COMPARE(fixture.controller.messages().size(), 1);
            QVERIFY(!fixture.controller.error().contains("Could not load messages"));
        } else {
            QCOMPARE(revalidated, 0);
        }
        const auto previousSockets = gateway.sockets.size();
        QVERIFY(gateway.sockets.last());
        gateway.sockets.last()->close();
        QTRY_VERIFY_WITH_TIMEOUT(gateway.sockets.size() > previousSockets
            && fixture.controller.syncState() == "live", 5000);
        QCOMPARE(revalidated, !leaveRoute && !forbidden ? 1 : 0);
    }

    void sameOwnerRenewalReloadsCancelledRouteRequestsAndPreservesDraft()
    {
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        int refreshes = 0;
        fixture.authentication = [&refreshes](const Request &request) {
            auto response = sessionResponse(request);
            if (request.body.value("grant_type") == "refresh_token") {
                auto token = response.data.toObject();
                token.insert("access_token", QString("project-token-%1").arg(++refreshes));
                response.data = token;
            }
            return response;
        };
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live");
        fixture.controller.setDraft("Unsent before token renewal");
        int topicRequests = 0;
        fixture.topics = [&topicRequests](const Request &) {
            const bool pending = ++topicRequests == 1;
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Recovered topic"}}}, 200, pending ? 500 : 0, {}};
        };
        fixture.controller.selectStream(streamId);
        QTRY_COMPARE(topicRequests, 1);
        fixture.session.requestRefresh();
        QTRY_COMPARE(refreshes, 2);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.topics().size(), 1, 1500);
        QCOMPARE(fixture.controller.topics().first().toMap().value("name").toString(), "Recovered topic");
        fixture.controller.selectTopic(topicId);
        QCOMPARE(fixture.controller.draft(), "Unsent before token renewal");
    }
    // Additive private slots for NativeControllerTest. SOURCE-ONLY / NOT RUN.
    // Reflection keeps the missing-capability RED executable on the old core.
    void historyCommitCountsAcceptedEmptyHistoryBeforeTheModelSignal()
    {
        Fixture f;
        f.aggregateMessages = true;
        f.messages = [](const Request &) { return Response{QJsonArray{}, 200, 1200}; };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(c.property("messageHistoryReady").isValid());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(0));
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QVector<qulonglong> published;
        const auto connection = connect(&c, &NativeController::messagesChanged, this, [&] {
            if (c.selectedTopic() == topicId && c.property("messageHistoryReady").toBool())
                published.append(c.property("historyCommitRevision").toULongLong());
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        c.login(f.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&] { return c.projects().size() == 1; }, 2000));
        c.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return c.streams().size() == 1; }, 2000));
        c.selectStream(streamId);
        QVERIFY(QTest::qWaitFor([&] { return !c.topics().isEmpty(); }, 2000));
        c.selectTopic(topicId); // Keep the current delayed HTTP history in flight.
        const auto pending = [&]() -> QNetworkReply * {
            for (auto *reply : f.api.findChildren<QNetworkReply *>())
                if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/messages/"
                    && QUrlQuery(reply->url()).queryItemValue("topic_uuid") == topicId) return reply;
            return nullptr;
        };
        QTRY_VERIFY(pending()); // Actual requested current history, not a sleep.
        QVERIFY(c.messages().isEmpty());
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(0));
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(1));
        QVERIFY(!published.isEmpty());
        QCOMPARE(published.first(), qulonglong(1)); // Counter visible inside publication.
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(1));
    }

    void historyCommitDoesNotCountLiveRowsOrOlderAdmission()
    {
        Fixture f;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 120; ++n) rows.insert(n, historyMessage(n));
        f.messages = [&](const Request &request) { return historyResponse(rows, request); };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(f.start());
        QTRY_COMPARE(c.messages().size(), 50);
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        const auto first = c.property("historyCommitRevision").toULongLong();
        const auto historyGets = f.totalHistoryGets();
        const auto live = historyMessage(121);
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.created", live)));
        QCOMPARE(c.messages().size(), 51);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first);
        QCOMPARE(f.totalHistoryGets(), historyGets);
        QVERIFY(c.property("messageHistoryReady").toBool());
        c.loadOlder();
        // Pagination admission/loading retains the accepted current baseline.
        QVERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first);
        QTRY_COMPARE(c.messages().size(), 101);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first + 1);
        QCOMPARE(f.totalHistoryGets(), historyGets + 1);
        QVERIFY(c.property("messageHistoryReady").toBool());
    }

    void historyCommitIgnoresTheOldRouteReplyAndRouteClear()
    {
        Fixture f; f.extraTopic = true;
        f.messages = [](const Request &request) {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("Owned " + topic,
                topic == topicId ? messageId : returnedId, topic)}, 200, topic == topicId ? 1200 : 0};
        };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        c.login(f.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&] { return c.projects().size() == 1; }, 2000));
        c.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return c.streams().size() == 1; }, 2000));
        c.selectStream(streamId);
        QVERIFY(QTest::qWaitFor([&] { return !c.topics().isEmpty(); }, 2000));
        c.selectTopic(topicId); // Keep the current delayed HTTP history in flight.
        const auto oldReply = [&]() -> QNetworkReply * {
            for (auto *reply : f.api.findChildren<QNetworkReply *>())
                if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/messages/"
                    && QUrlQuery(reply->url()).queryItemValue("topic_uuid") == topicId) return reply;
            return nullptr;
        };
        QTRY_VERIFY(oldReply());
        const auto before = c.property("historyCommitRevision").toULongLong();
        c.selectTopic(secondTopicId);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), before);
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QTRY_COMPARE(modelMessage(c, returnedId).value("content").toString(), "Owned " + secondTopicId);
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        const auto current = c.property("historyCommitRevision").toULongLong();
        QCOMPARE(current, before + 1);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), current);
        QVERIFY(modelMessage(c, messageId).isEmpty());
        c.selectStream({});
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), current);
        QVERIFY(!c.property("messageHistoryReady").toBool());
    }

    void historyCommitCountsWarmCacheAndStaleHydrationWithoutClaimingReadiness()
    {
        QTemporaryDir directory; qint64 now = 1000;
        NativeCacheService cache(directory.path(), {}, true, [&] { return now; });
        Fixture f; f.extraTopic = true; f.controller.useCache(&cache);
        bool fail = false;
        f.messages = [&](const Request &request) {
            if (fail) return Response{QJsonObject{}, 503, 100};
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message(topic, topic == topicId ? messageId : returnedId, topic)}};
        };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(f.start()); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QVERIFY(flushCache(cache));
        const auto first = c.property("historyCommitRevision").toULongLong();
        c.selectTopic(secondTopicId); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QVERIFY(flushCache(cache));
        const auto gets = f.topicHistoryGets;
        c.selectTopic(topicId); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first + 2);
        QCOMPARE(f.topicHistoryGets, gets); // Actual fresh-cache publication.
        now += 120001; fail = true;
        const auto warm = c.property("historyCommitRevision").toULongLong();
        c.selectTopic(secondTopicId);
        QTRY_COMPARE(modelMessage(c, returnedId).value("content").toString(), secondTopicId);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), warm + 1);
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QTRY_VERIFY(!c.error().isEmpty());
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), warm + 1);
    }

    void historyCommitRejectsAnOlderSnapshotAfterLiveRevisionAndCountsOnlyReplacement()
    {
        Fixture f;
        auto row = historyMessage(1);
        bool delayed = false;
        f.messages = [&](const Request &) { return Response{QJsonArray{row}, 200, delayed ? 1200 : 0}; };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(f.start()); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        const auto initial = c.property("historyCommitRevision").toULongLong();
        const auto gets = f.topicHistoryGets;
        delayed = true; c.refresh();
        QTRY_COMPARE(f.topicHistoryGets, gets + 1);
        QPointer<QNetworkReply> old;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/messages/"
                && QUrlQuery(reply->url()).queryItemValue("topic_uuid") == topicId) old = reply;
        QVERIFY(old);
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Newer live body"}};
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.updated", row)));
        QCOMPARE(modelMessage(c, row.value("uuid").toString()).value("content").toString(), QString("Newer live body"));
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), initial);
        delayed = false;
        QTRY_COMPARE(f.topicHistoryGets, gets + 2); // Existing revision fence revalidates.
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), initial + 1);
        QCOMPARE(modelMessage(c, row.value("uuid").toString()).value("content").toString(), QString("Newer live body"));
    }

    // ---- Anchor boundary paging (issue #71, docs/NATIVE_DESKTOP_ANCHOR_PAGING.md) ----

    static QJsonObject anchoredHistoryRow(int number)
    {
        const auto id = QStringLiteral("8e8e8e8e-8e8e-4e8e-8e8e-")
            + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
        auto row = message(QStringLiteral("Anchor history %1").arg(number), id);
        row.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                         .addSecs(number).toString(Qt::ISODateWithMs));
        return row;
    }

    void conversationOpeningResolvesServerBoundary_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"unread", "saved", "latest", "latest-failed", "cancel-lookup", "cancel-around",
                 "session-around", "read-lookup", "read-around", "saved-read-around", "saved-history-around", "malformed", "foreign", "empty", "inconsistent-empty", "inconsistent-later"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void conversationOpeningResolvesServerBoundary()
    {
        QFETCH(QString, scenario);
        Fixture fixture; fixture.aggregateMessages = true;
        const auto row = [](int number) { auto value = anchoredHistoryRow(number); value["read"] = false; return value; };
        const auto id = [&](int number) { return row(number).value("uuid").toString(); };
        int lookups = 0, aroundPages = 0, latestPages = 0;
        bool held = false, queryContract = true;
        std::vector<std::function<void()>> release;
        fixture.messages = [&](const Request &request) -> Response {
            const QUrlQuery query(request.target);
            if (query.hasQueryItem("uuid")) { auto saved = row(20); saved["read"] = true; return {QJsonArray{saved}}; }
            if (query.queryItemValue("read") == "false") {
                ++lookups;
                queryContract = queryContract && query.queryItemValue("sort_dir") == "asc"
                    && query.queryItemValue("page_limit") == "1";
                QJsonArray rows{row(scenario.startsWith("read-") && lookups > 1 ? 21 : 20)};
                if (lookups == 1 && scenario == "inconsistent-empty") rows = {};
                if (lookups == 1 && scenario == "inconsistent-later") rows = {row(119)};
                if (scenario == "malformed") rows.append(row(21));
                if (scenario == "foreign") { auto value = row(20); value["project_id"] = secondTopicId; rows = {value}; }
                if (scenario == "empty") rows = {};
                Response response{rows};
                if (!held && (scenario == "cancel-lookup" || scenario == "read-lookup")) {
                    held = true; response.defer = [&](std::function<void()> deliver) { release.push_back(std::move(deliver)); };
                }
                return response;
            }
            const auto marker = query.queryItemValue("page_marker");
            if (!marker.isEmpty()) {
                ++aroundPages;
                const bool newer = query.queryItemValue("sort_dir") == "asc";
                Response response{QJsonArray{row(newer ? 22 : 19)}};
                if (newer) response.headers["X-Pagination-Marker"] = id(22).toUtf8();
                if (!held && (scenario == "cancel-around" || scenario == "session-around" || scenario == "read-around"
                    || scenario == "saved-read-around" || scenario == "saved-history-around")) {
                    // Hold both pages until the test retires this opening.
                    response.defer = [&](std::function<void()> deliver) { release.push_back(std::move(deliver)); };
                }
                return response;
            }
            ++latestPages;
            if (scenario == "latest-failed" && latestPages > 2) return {QJsonObject{}, 503};
            auto latest = row(119), previous = row(118);
            if (scenario == "empty") { latest["read"] = true; previous["read"] = true; }
            return {QJsonArray{latest, previous}, 200, 0, {{"X-Pagination-Marker", id(118).toUtf8()}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.prepareConversationOpening(scenario.startsWith("saved") ? id(20) : QString{});
        if (scenario.startsWith("cancel-") || scenario.startsWith("read-") || scenario == "session-around"
            || scenario == "saved-read-around" || scenario == "saved-history-around") {
            QTRY_COMPARE(release.size(), scenario.endsWith("around") ? size_t(2) : size_t(1));
            QVERIFY(fixture.controller.conversationOpening().value("pending").toBool());
            if (scenario.startsWith("cancel-")) fixture.controller.cancelConversationOpening();
            else if (scenario == "session-around") fixture.api.setAccessToken("replacement-synthetic-token");
            else QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read",
                QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{id(scenario == "saved-history-around" ? 119 : 20)}}})));
            held = true;
            const auto pending = std::move(release);
            for (const auto &deliver : pending) deliver();
            if (scenario.startsWith("cancel-") || scenario == "session-around") {
                QTest::qWait(150);
                QCOMPARE(fixture.controller.messages().size(), 2);
                QVERIFY(!fixture.controller.busy());
                QVERIFY(!fixture.controller.conversationOpening().value("pending").toBool());
                return;
            }
        }
        if (scenario == "malformed" || scenario == "foreign") {
            QTRY_VERIFY(fixture.controller.conversationOpening().value("failed").toBool());
            QVERIFY(!fixture.controller.conversationOpening().value("ready").toBool());
            QCOMPARE(fixture.controller.messages().size(), 2); QCOMPARE(aroundPages, 0);
            return;
        }
        QTRY_VERIFY(fixture.controller.conversationOpening().value("ready").toBool());
        QCOMPARE(fixture.controller.conversationOpening().value("messageId").toString(),
            scenario == "empty" ? QString{} : id(scenario.startsWith("read-") ? 21 : 20));
        QVERIFY(queryContract);
        QVERIFY(fixture.controller.anchorMessageId().isEmpty()); // Opening never flashes an explicit-link highlight.
        if (scenario == "empty") { QCOMPARE(aroundPages, 0); return; }
        QVERIFY(modelMessage(fixture.controller, id(119)).isEmpty());
        QVERIFY(fixture.controller.canLoadNewer());
        if (scenario.startsWith("saved")) { QCOMPARE(lookups, 0); QVERIFY(modelMessage(fixture.controller, id(20)).value("read").toBool()); }
        if (scenario.startsWith("latest")) {
            fixture.controller.loadLatestMessages();
            QTRY_VERIFY(!fixture.controller.busy());
            if (scenario == "latest") {
                QCOMPARE(fixture.controller.messages().size(), 2);
                QVERIFY(!modelMessage(fixture.controller, id(119)).isEmpty());
                QVERIFY(!fixture.controller.canLoadNewer());
            } else {
                QVERIFY(!modelMessage(fixture.controller, id(20)).isEmpty());
                QVERIFY(fixture.controller.canLoadNewer());
            }
        }
    }

    void savedAnchorRetryKeepsDeletionAndLifecycleBounds_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"deleted", "foreign", "churn", "cancel", "session"})
            QTest::newRow(scenario) << QString(scenario);
    }
    void savedAnchorRetryKeepsDeletionAndLifecycleBounds()
    {
        QFETCH(QString, scenario);
        Fixture fixture; fixture.aggregateMessages = true;
        const auto row = [](int number) { auto value = anchoredHistoryRow(number); value["read"] = true; return value; };
        const auto id = [&](int number) { return row(number)["uuid"].toString(); };
        int exactLookups = 0, unreadLookups = 0, aroundPages = 0;
        std::vector<std::function<void()>> release;
        const auto cleanup = qScopeGuard([&] { for (const auto &deliver : release) deliver(); });
        fixture.messages = [&](const Request &request) -> Response {
            const QUrlQuery query(request.target);
            if (query.hasQueryItem("uuid")) {
                ++exactLookups;
                if (exactLookups > 1 && scenario == "deleted") return {QJsonObject{}, 404};
                auto saved = row(20);
                if (exactLookups > 1 && scenario == "foreign") saved["project_id"] = secondTopicId;
                return {QJsonArray{saved}};
            }
            if (query.queryItemValue("read") == "false") {
                ++unreadLookups; auto unread = row(21); unread["read"] = false;
                return {QJsonArray{unread}};
            }
            if (!query.queryItemValue("page_marker").isEmpty()) {
                ++aroundPages;
                Response response{QJsonArray{row(query.queryItemValue("sort_dir") == "asc" ? 22 : 19)}};
                if (aroundPages <= 2 || ((scenario == "churn" || scenario == "cancel" || scenario == "session") && aroundPages <= 4))
                    response.defer = [&](auto deliver) { release.push_back(std::move(deliver)); };
                return response;
            }
            return {QJsonArray{row(119)}, 200, 0, {{"X-Pagination-Marker", id(119).toUtf8()}}};
        };
        QVERIFY(fixture.start()); fixture.controller.prepareConversationOpening(id(20));
        QTRY_COMPARE(release.size(), size_t(2));
        const auto historyChange = [&] {
            return fixture.controller.applyRealtimeEvent(controllerEvent("messages.read",
                QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{id(119)}}}));
        };
        const auto deliver = [&] { auto pending = std::move(release); release.clear(); for (const auto &send : pending) send(); };
        QVERIFY(historyChange()); deliver();
        if (scenario == "churn" || scenario == "cancel" || scenario == "session") {
            QTRY_COMPARE(release.size(), size_t(2));
            if (scenario == "churn") QVERIFY(historyChange());
            else if (scenario == "cancel") fixture.controller.cancelConversationOpening();
            else fixture.api.setAccessToken("replacement-synthetic-token");
            deliver();
        }
        QTRY_VERIFY(!fixture.controller.busy());
        QTRY_VERIFY(!fixture.controller.conversationOpening().value("pending").toBool());
        QCOMPARE(exactLookups, 2);
        QCOMPARE(unreadLookups, scenario == "deleted" ? 1 : 0);
        if (scenario == "deleted") {
            QVERIFY(fixture.controller.conversationOpening().value("ready").toBool());
            QCOMPARE(fixture.controller.conversationOpening().value("messageId").toString(), id(21));
        } else {
            QVERIFY(!modelMessage(fixture.controller, id(119)).isEmpty());
            QVERIFY(modelMessage(fixture.controller, id(20)).isEmpty());
            if (scenario == "foreign" || scenario == "churn")
                QVERIFY(fixture.controller.conversationOpening().value("failed").toBool());
        }
    }

    // A deep-link anchor below the bounded older walk's reach opens the reference's
    // around-anchor window (desc + asc pages at page_marker=<anchor>, see
    // workspace_ui 61dacef7 messenger-messages.api.test.ts:169-246): the anchor sits
    // mid-window with newer rows below it, and the newer boundary pages outward.
    void anchorMidHistoryOpensWindowWithNewerRows()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString anchor = anchoredHistoryRow(50).value("uuid").toString();
        int olderWalks = 0;
        int ascAnchorPages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{anchoredHistoryRow(50)}};
            const auto dir = query.queryItemValue("sort_dir");
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) {
                QJsonArray tail;
                for (int number = 500; number >= 451; --number) tail.append(anchoredHistoryRow(number));
                Response response{tail, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker",
                    anchoredHistoryRow(451).value("uuid").toString().toUtf8());
                return response;
            }
            if (dir == "asc" && marker == anchor) {
                ++ascAnchorPages;
                QJsonArray page;
                for (int number = 51; number <= 100; ++number) page.append(anchoredHistoryRow(number));
                Response response{page, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker", "newer-2");
                return response;
            }
            if (dir == "desc" && marker == anchor) {
                QJsonArray page;
                for (int number = 49; number >= 1; --number) page.append(anchoredHistoryRow(number));
                return Response{page}; // the older end is reached: no marker
            }
            if (dir == "asc" && marker == "newer-2") {
                QJsonArray page;
                for (int number = 101; number <= 120; ++number) page.append(anchoredHistoryRow(number));
                return Response{page}; // the newer end is reached: no marker
            }
            ++olderWalks; // the bounded walk must not run when the around window succeeds
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + anchor);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), anchor);
        QVERIFY(ascAnchorPages >= 1); // the around-anchor newer page was requested
        QCOMPARE(olderWalks, 0);
        QVERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(55).value("uuid").toString()).isEmpty());
        QVERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(45).value("uuid").toString()).isEmpty());
        QVERIFY(modelMessage(fixture.controller, anchoredHistoryRow(500).value("uuid").toString()).isEmpty());
        QVERIFY(fixture.controller.canLoadNewer());
        QVERIFY(!fixture.controller.canLoadOlder()); // the older page carried no marker
        fixture.controller.loadNewer();
        QTRY_VERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(120).value("uuid").toString()).isEmpty());
        QCOMPARE(fixture.controller.anchorMessageId(), anchor); // highlight survives newer paging
        QVERIFY(!fixture.controller.canLoadNewer()); // the newer page carried no marker
    }

    // exo-review (#140): a malformed row on an around-anchor page must reject that
    // page (markers are only valid if its rows were accepted) and fall back to the
    // bounded older walk instead of committing a window with a silent gap.
    void malformedAroundPageFallsBackToOlderWalk()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString anchor = anchoredHistoryRow(50).value("uuid").toString();
        int aroundCommits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{anchoredHistoryRow(50)}};
            const auto dir = query.queryItemValue("sort_dir");
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) {
                QJsonArray tail;
                for (int number = 500; number >= 451; --number) tail.append(anchoredHistoryRow(number));
                Response response{tail, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker", "tail-1");
                return response;
            }
            if (dir == "asc" && marker == anchor)
                return Response{QJsonArray{anchoredHistoryRow(51)}, 200, 0, {{"X-Pagination-Marker", "newer-2"}}};
            if (dir == "desc" && marker == anchor) {
                ++aroundCommits;
                QJsonArray page;
                page.append(QJsonObject{{"uuid", "not-a-uuid"}, {"project_id", projectId}});
                return Response{page};
            }
            if (dir == "asc" && marker == "newer-2")
                return Response{QJsonArray{}};
            // bounded older walk path: the target sits on the first older page
            return Response{QJsonArray{anchoredHistoryRow(50), anchoredHistoryRow(49)}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + anchor);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), anchor); // reached via the walk
        QCOMPARE(aroundCommits, 1); // the around page was requested once and rejected
        // The tail window survived: the malformed around page did not replace it.
        QVERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(500).value("uuid").toString()).isEmpty());
        QVERIFY(!fixture.controller.canLoadNewer()); // no newer boundary from a rejected window
    }

    void tailWindowHasNoNewerBoundary()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        int ascPages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).queryItemValue("sort_dir") == "asc") {
                ++ascPages;
                return Response{QJsonArray{}};
            }
            return Response{QJsonArray{message("Current body")}};
        };
        QVERIFY(fixture.start());
        QTest::qWait(200);
        QVERIFY(!fixture.controller.canLoadNewer()); // a tail window has no newer side
        fixture.controller.loadNewer();
        QTest::qWait(200);
        QCOMPARE(ascPages, 0); // loadNewer stays inert without a newer boundary
    }

    void newerPageFailureKeepsWindowAndAnchor()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString anchor = anchoredHistoryRow(50).value("uuid").toString();
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{anchoredHistoryRow(50)}};
            const auto dir = query.queryItemValue("sort_dir");
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) {
                QJsonArray tail;
                for (int number = 500; number >= 451; --number) tail.append(anchoredHistoryRow(number));
                return Response{tail};
            }
            if (dir == "asc" && marker == anchor) {
                QJsonArray page;
                for (int number = 51; number <= 100; ++number) page.append(anchoredHistoryRow(number));
                Response response{page, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker", "newer-2");
                return response;
            }
            if (dir == "desc" && marker == anchor)
                return Response{QJsonArray{anchoredHistoryRow(49)}};
            if (dir == "asc" && marker == "newer-2")
                return Response{QJsonObject{{"error", "boom"}}, 500, 0, {}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + anchor);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), anchor);
        QTRY_VERIFY(fixture.controller.canLoadNewer());
        fixture.controller.loadNewer();
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.anchorMessageId(), anchor);
        QVERIFY(fixture.controller.canLoadNewer()); // the marker is kept for a retry
        QVERIFY(modelMessage(fixture.controller, anchoredHistoryRow(101).value("uuid").toString()).isEmpty());
    }

    // Issue #156 (docs/NATIVE_DESKTOP_HISTORY_PREFETCH.md): the reserve-based
    // prefetch trigger lives in the controller so the trigger threshold, the
    // in-flight coalescing and the end-of-history guard are testable without
    // a window. Reserve policy: max(100 px reference LOAD_MORE_THRESHOLD
    // boundary floor, viewport height); arm at >= reserve, fire at <= reserve
    // (inclusive comparisons mirror workspace-message-list-scroll.hook.ts
    // :1172-1197 at 61dacef78dab2f5d0be76f283bebf49fe29b84af).
    void historyPrefetchFiresWhileReserveRemains()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 150; ++n) rows.insert(n, historyMessage(n));
        QVector<QByteArray> olderMarkers;
        fixture.messages = [&](const Request &request) {
            const auto response = historyResponse(rows, request);
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("topic_uuid") && query.hasQueryItem("page_marker"))
                olderMarkers.append(query.queryItemValue("page_marker").toUtf8());
            return response;
        };
        QVERIFY(fixture.start());
        auto &c = fixture.controller;
        QCOMPARE(c.messages().size(), 50);
        const int baseline = fixture.totalHistoryGets();
        QVERIFY(c.canLoadOlder());
        const qreal viewport = 600.0;
        // At or above the reserve the note only arms: opening a tail window or
        // scrolling down must never fetch older pages. One pixel above the
        // reserve is the strict-arming case; the exact reserve boundary is
        // the reference's inclusive arm-and-fire case proven by
        // historyPrefetchFiresAtExactReserveBoundary.
        c.noteScrollReserve(2400, viewport);
        c.noteScrollReserve(viewport + 1, viewport);
        QTest::qWait(250);
        QCOMPARE(fixture.totalHistoryGets(), baseline);
        // One pixel below the reserve: content is still above the viewport and
        // the request has to start now, not when the boundary is reached.
        const auto oldestBeforePrefetch = c.messages().first().toMap().value("id").toString().toUtf8();
        c.noteScrollReserve(viewport - 1, viewport);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 1);
        QCOMPARE(olderMarkers.at(0), oldestBeforePrefetch);
        QTRY_COMPARE(c.messages().size(), 100);
        QTRY_VERIFY(!c.busy());
        // One request per descent: the page that just landed does not chain a
        // second fetch while the viewport stays below the reserve.
        c.noteScrollReserve(0, viewport);
        c.noteScrollReserve(1, viewport);
        QTest::qWait(250);
        QCOMPARE(fixture.totalHistoryGets(), baseline + 1);
        // A direction change back below the reserve fetches the next page.
        c.noteScrollReserve(3000, viewport);
        c.noteScrollReserve(10, viewport);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 2);
        QTRY_COMPARE(c.messages().size(), 150);
    }

    void fastHistoryScrollPrefetchesBeforeItsMeasuredPageWait()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 150; ++n) rows.insert(n, historyMessage(n));
        int olderRequests = 0;
        fixture.messages = [&](const Request &request) {
            auto response = historyResponse(rows, request);
            const QUrlQuery query(request.target);
            if (query.hasQueryItem("topic_uuid")) {
                response.delay = 500;
                if (query.hasQueryItem("page_marker")) ++olderRequests;
            }
            return response;
        };
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        QCOMPARE(controller.messages().size(), 50);
        // The hosted trace consumes 100 px per wheel notch, about 25 ms apart.
        // At the old one-viewport threshold the top was reached before the response.
        for (int distance = 2100; distance >= 1100; distance -= 100) {
            controller.noteScrollReserve(distance, 670);
            QTest::qWait(25);
        }
        QCOMPARE(olderRequests, 1);
        QTRY_COMPARE(controller.messages().size(), 100);
        QCOMPARE(olderRequests, 1);
    }

    void historyPrefetchFiresAtExactReserveBoundary()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 150; ++n) rows.insert(n, historyMessage(n));
        fixture.messages = [&](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        auto &c = fixture.controller;
        const qreal viewport = 600.0;
        const int baseline = fixture.totalHistoryGets();
        // First descent starts the prefetch just below the reserve (the arm is
        // consumed when the request begins); wait for the page so the older
        // boundary is requestable again.
        c.noteScrollReserve(viewport - 1, viewport);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 1);
        QTRY_COMPARE(c.messages().size(), 100);
        QTRY_VERIFY(!c.busy());
        QVERIFY(c.canLoadOlder());
        // Reviewer #212: the reference comparisons are both inclusive (re-arm
        // at >= reserve, trigger at <= reserve), so a note landing exactly at
        // the reserve must re-arm and fire in the same call - a scroll that
        // stops right at the boundary may never leave the next page un-fetched.
        c.noteScrollReserve(viewport, viewport);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 2);
        QTRY_COMPARE(c.messages().size(), 150);
        QTRY_VERIFY(!c.busy());
    }

    void historyPrefetchRearmNoteArmsWithoutRequesting()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 150; ++n) rows.insert(n, historyMessage(n));
        fixture.messages = [&](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        auto &c = fixture.controller;
        const qreal viewport = 600.0;
        const int baseline = fixture.totalHistoryGets();
        // First descent consumes the arm; after the page lands the controller
        // stays disarmed (an older commit is not a re-arm site), so any descent
        // note below the reserve - the state a suppressed viewport restore can
        // leave behind - is silently dropped.
        c.noteScrollReserve(viewport - 1, viewport);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 1);
        QTRY_COMPARE(c.messages().size(), 100);
        QTRY_VERIFY(!c.busy());
        c.noteScrollReserve(5, viewport);
        QTest::qWait(250);
        QCOMPARE(fixture.totalHistoryGets(), baseline + 1);
        // The view's re-arm-only note (restored geometry above the reserve)
        // re-arms without ever starting a request.
        c.noteScrollRearm(viewport, viewport);
        QTest::qWait(250);
        QCOMPARE(fixture.totalHistoryGets(), baseline + 1);
        // The next user-attributed descent below the reserve now starts the
        // next page.
        c.noteScrollReserve(viewport - 1, viewport);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 2);
        QTRY_COMPARE(c.messages().size(), 150);
        QTRY_VERIFY(!c.busy());
        // A re-arm note below the reserve keeps the policy disarmed: restored
        // geometry inside the reserve arms nothing on its own.
        c.noteScrollRearm(1, viewport);
        c.noteScrollReserve(2, viewport);
        QTest::qWait(250);
        QCOMPARE(fixture.totalHistoryGets(), baseline + 2);
    }

    void historyPrefetchCoalescesInFlight()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 150; ++n) rows.insert(n, historyMessage(n));
        bool holdNext = true;
        std::function<void()> releaseOld;
        auto releaseOnExit = qScopeGuard([&] { if (releaseOld) releaseOld(); });
        fixture.messages = [&](const Request &request) {
            auto response = historyResponse(rows, request);
            if (!QUrlQuery(request.target).queryItemValue("page_marker").isEmpty() && holdNext) {
                holdNext = false;
                response.defer = [&](std::function<void()> send) { releaseOld = std::move(send); };
            }
            return response;
        };
        QVERIFY(fixture.start());
        auto &c = fixture.controller;
        const int baseline = fixture.totalHistoryGets();
        c.noteScrollReserve(300, 600.0);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 1);
        QTRY_VERIFY(bool(releaseOld)); // the response is held by the test gate
        // In flight and still below the reserve: descents and re-arm-plus-
        // descent cycles must coalesce onto the one held request.
        c.noteScrollReserve(0, 600.0);
        c.noteScrollReserve(700, 600.0);
        c.noteScrollReserve(0, 600.0);
        QTest::qWait(250);
        QCOMPARE(fixture.totalHistoryGets(), baseline + 1);
        QCOMPARE(c.messages().size(), 50);
        QVERIFY(!c.canLoadOlder()); // the in-flight guard is observable and stable
        auto send = std::move(releaseOld); releaseOld = {};
        send(); // release the held older page
        QTRY_COMPARE(c.messages().size(), 100);
        QVERIFY(c.canLoadOlder());
        // After the release the traversal continues: one request per descent.
        c.noteScrollReserve(5000, 600.0);
        c.noteScrollReserve(1, 600.0);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 2);
        QTRY_COMPARE(c.messages().size(), 150);
    }

    void historyPrefetchStopsAtEndOfHistory()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 60; ++n) rows.insert(n, historyMessage(n));
        fixture.messages = [&](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        auto &c = fixture.controller;
        const int baseline = fixture.totalHistoryGets();
        QVERIFY(c.canLoadOlder());
        // The reference boundary case still fires at the very top (distance 0).
        c.noteScrollReserve(0, 600.0);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 1);
        QTRY_COMPARE(c.messages().size(), 60);
        QTRY_VERIFY(!c.canLoadOlder()); // the final page carried no older marker
        const int gets = fixture.totalHistoryGets();
        for (int i = 0; i < 5; ++i)
            c.noteScrollReserve(0, 600.0);
        QTest::qWait(250);
        QCOMPARE(fixture.totalHistoryGets(), gets); // end of history never loops
    }

    void historyPrefetchFailedPageStaysRetryable()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 150; ++n) rows.insert(n, historyMessage(n));
        bool failOlder = true;
        fixture.messages = [&](const Request &request) {
            if (!QUrlQuery(request.target).queryItemValue("page_marker").isEmpty() && failOlder)
                return Response{QJsonObject{}, 500};
            return historyResponse(rows, request);
        };
        QVERIFY(fixture.start());
        auto &c = fixture.controller;
        const int baseline = fixture.totalHistoryGets();
        c.noteScrollReserve(50, 600.0);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 1);
        // A failed page leaves the loaded chat usable: nothing committed, the
        // busy state released, the older boundary kept for a retry.
        QTRY_VERIFY(!c.busy());
        QCOMPARE(c.messages().size(), 50);
        QVERIFY(c.canLoadOlder());
        // The failure re-armed the prefetch; the next descent retries once -
        // and with the server healthy the page lands.
        failOlder = false;
        c.noteScrollReserve(30, 600.0);
        QTRY_COMPARE(fixture.totalHistoryGets(), baseline + 2);
        QTRY_COMPARE(c.messages().size(), 100);
    }
};

QTEST_MAIN(ControllerHistoryTest)
#include "native-controller-history-test.moc"
