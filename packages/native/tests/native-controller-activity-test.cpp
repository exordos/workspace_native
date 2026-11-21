#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"
#include "support/controller-folder-fixtures.h"
#include "support/controller-mention-fixtures.h"

inline const QString selfStreamId = "d0d0d0d0-d0d0-4d0d-8d0d-000000000001";
inline const QString selfDefaultTopic = "d1d1d1d1-d1d1-4d1d-8d1d-111111111111";

QJsonObject selfChatStreamDto()
{
    auto row = creationChannel(selfStreamId, "Personal notes");
    row["private"] = true;
    row["direct_user_uuid"] = userId;
    row["default_topic_uuid"] = selfDefaultTopic;
    return row;
}

QJsonObject selfDefaultTopicDto()
{
    auto row = creationTopic(selfDefaultTopic, selfStreamId, "Personal notes");
    row["is_default"] = true;
    return row;
}

class ControllerActivityTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Activity mentions page (issue #42, docs/NATIVE_DESKTOP_ACTIVITY_MENTIONS.md) ----

    void activityMentionCollectionsIssueServerFilteredQueries()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1, true)}, {2, mentionMessage(2)}, {3, mentionMessage(3)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.mentions = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        QVERIFY(!fixture.controller.mentionsVisible());
        QCOMPARE(fixture.controller.mentionsFilter(), QString("all"));
        fixture.controller.showMentions();
        QVERIFY(fixture.controller.mentionsVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);

        // The All collection query is mentioned-scoped without a read filter.
        // Measured in workspace_ui activity-mentions.api.ts:70-77.
        const auto mentionsAll = [&] {
            for (const auto &query : *queries)
                if (query.queryItemValue("page_limit") == "50"
                    && query.queryItemValue("mentioned") == "true") return query;
            return QUrlQuery{};
        }();
        QVERIFY(mentionsAll.hasQueryItem("page_limit"));
        QCOMPARE(mentionsAll.queryItemValue("mentioned"), QStringLiteral("true"));
        QVERIFY(!mentionsAll.hasQueryItem("read"));
        QCOMPARE(mentionsAll.queryItemValue("sort_key"), QStringLiteral("created_at"));
        QCOMPARE(mentionsAll.queryItemValue("sort_dir"), QStringLiteral("desc"));
        // The unread-mentions index prefetch walks 100 per page (api.ts:21,110-121).
        const bool hasIndexWalk = std::any_of(queries->cbegin(), queries->cend(),
            [](const QUrlQuery &query) { return query.queryItemValue("page_limit") == "100"
                && query.queryItemValue("read") == "false"; });
        QVERIFY(hasIndexWalk);

        auto page = fixture.controller.mentionsPage();
        auto list = page.value("rows").toList();
        QCOMPARE(list.size(), 3);
        // Newest first (activity-page.ui.tsx:239 sortUniqueActivityMessages reverse).
        QCOMPARE(list.first().toMap().value("id").toString(),
            mentionMessage(3).value("uuid").toString());
        // Index owns the dot: only the two unread rows light up (tests 590, 633).
        QVERIFY(list.at(0).toMap().value("unread").toBool());
        QVERIFY(list.at(1).toMap().value("unread").toBool());
        QVERIFY(!list.at(2).toMap().value("unread").toBool());

        // Unread is a separate server-filtered collection (activity-mentions.api.ts:73).
        queries->clear();
        fixture.controller.selectMentionsFilter("unread");
        QCOMPARE(fixture.controller.mentionsFilter(), QString("unread"));
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);
        const auto mentionsUnread = [&] {
            for (const auto &query : *queries)
                if (query.queryItemValue("page_limit") == "50"
                    && query.queryItemValue("mentioned") == "true") return query;
            return QUrlQuery{};
        }();
        QVERIFY(mentionsUnread.hasQueryItem("page_limit"));
        QCOMPARE(mentionsUnread.queryItemValue("mentioned"), QStringLiteral("true"));
        QCOMPARE(mentionsUnread.queryItemValue("read"), QStringLiteral("false"));
        list = fixture.controller.mentionsPage().value("rows").toList();
        QCOMPARE(list.size(), 2);
        QVERIFY(list.at(0).toMap().value("unread").toBool());
        QVERIFY(list.at(1).toMap().value("unread").toBool());

        // Every filter entry is a fresh first page: no page_marker leaks across
        // collections (activity-page.ui.tsx:283-297 collectionKey reset).
        fixture.controller.selectMentionsFilter("all");
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);
        for (const auto &query : *queries)
            if (query.queryItemValue("page_limit") == "50" && query.queryItemValue("mentioned") == "true")
                QVERIFY(!query.hasQueryItem("page_marker"));
        QCOMPARE(fixture.controller.mentionsPage().value("rows").toList().size(), 3);
    }

    void activityMentionPaginationFollowsMarkerNewestFirst()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        auto rows = std::make_shared<QMap<int, QJsonObject>>();
        for (int n = 1; n <= 60; ++n)
            rows->insert(n, mentionMessage(n, true)); // read=true keeps the index walk empty
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.mentions = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 50, 4000);
        auto page = fixture.controller.mentionsPage();
        QVERIFY(page.value("canLoadMore").toBool());

        fixture.controller.loadMoreMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 60, 4000);
        // The continuation rode X-Pagination-Marker into page_marker (api-client.cpp:450).
        const bool markedRequest = std::any_of(queries->cbegin(), queries->cend(),
            [](const QUrlQuery &query) { return query.queryItemValue("page_limit") == "50"
                && query.hasQueryItem("page_marker"); });
        QVERIFY(markedRequest);
        const auto list = fixture.controller.mentionsPage().value("rows").toList();
        QCOMPARE(list.size(), 60);
        QSet<QString> ids;
        for (const auto &value : list) ids.insert(value.toMap().value("id").toString());
        QCOMPARE(ids.size(), 60); // dedupe by uuid (activity-page.ui.tsx:95-98)
        QCOMPARE(list.first().toMap().value("id").toString(), mentionMessage(60).value("uuid").toString());
        QCOMPARE(list.last().toMap().value("id").toString(), mentionMessage(1).value("uuid").toString());
        QVERIFY(!fixture.controller.mentionsPage().value("canLoadMore").toBool());
    }

    void sidebarMentionBadgeUpdatesOnExactRemoval_data()
    {
        QTest::addColumn<bool>("deleted");
        QTest::newRow("messages.read") << false;
        QTest::newRow("message.deleted") << true;
    }

    void sidebarMentionBadgeUpdatesOnExactRemoval()
    {
        QFETCH(bool, deleted);
        Fixture fixture;
        QVERIFY(fixture.start());
        bool displayed = false;
        // A sidebar delegate observes this notifier. Reading the projection only
        // after the event would conceal a missing notification.
        connect(&fixture.controller, &NativeController::sidebarStreamsChanged, this, [&] {
            for (const auto &row : fixture.controller.sidebarStreams()) {
                if (row.toMap().value("id").toString() == streamId)
                    displayed = row.toMap().value("hasMention").toBool();
            }
        });
        for (int n : {1, 2})
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", mentionMessage(n), n)));
        QVERIFY(displayed);
        for (int n : {1, 2}) {
            const auto uuid = mentionMessage(n).value("uuid");
            const auto kind = deleted ? QStringLiteral("message.deleted") : QStringLiteral("messages.read");
            const QJsonObject payload = deleted
                ? QJsonObject{{"kind", kind}, {"uuid", uuid}, {"stream_uuid", streamId}, {"topic_uuid", topicId}}
                : QJsonObject{{"kind", kind}, {"project_id", projectId}, {"message_uuids", QJsonArray{uuid}}};
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent(kind, payload, 2 + n)));
            QCOMPARE(displayed, n == 1);
        }
    }

    void sidebarMentionBootstrapPreservesConcurrentReadAndDelete_data()
    {
        QTest::addColumn<bool>("firstPageUnavailable");
        QTest::newRow("delayed-page") << false;
        QTest::newRow("temporary-failure") << true;
    }

    void sidebarMentionBootstrapPreservesConcurrentReadAndDelete()
    {
        QFETCH(bool, firstPageUnavailable);
        int attempts = 0;
        Fixture fixture;
        std::function<void()> release;
        fixture.mentions = [&](const Request &) {
            if (++attempts == 1 && firstPageUnavailable) return Response{QJsonObject{}, 503};
            Response result{QJsonArray{mentionMessage(1), mentionMessage(2), mentionMessage(3)}};
            result.defer = [&](std::function<void()> send) { release = std::move(send); };
            return result;
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(release);
        const auto read = mentionMessage(1).value("uuid");
        const auto deleted = mentionMessage(2).value("uuid");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read",
            {{"project_id", projectId}, {"message_uuids", QJsonArray{read}}}, 13)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted",
            {{"uuid", deleted}, {"stream_uuid", streamId}, {"topic_uuid", topicId}}, 14)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", mentionMessage(4), 15)));
        release();
        QTRY_COMPARE(fixture.controller.activityMentionsCount(), 2);
        QVERIFY(!fixture.controller.mentionsVisible());
    }

    void activityMentionRealtimeMergesWithoutRefetch()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2, false, true)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.mentions = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 2, 4000);
        const auto rowWith = [&](const QString &id) {
            for (const auto &value : fixture.controller.mentionsPage().value("rows").toList())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };
        // Own messages that mention the user are listed (activity-page.test.tsx:370).
        QVERIFY(rowWith(mentionMessage(2).value("uuid").toString()).contains("id"));
        const int fetchCount = queries->size();
        QVERIFY(fetchCount >= 1);

        // A live mention merges into the open page without refetching the page
        // (activity-page.test.tsx:489; classification via message.mentioned per
        // activity-realtime-applier.lib.ts:65-75 and :113-127).
        auto live = mentionMessage(90);
        live["author_uuid"] = QStringLiteral("66666666-6666-4666-8666-666666666666");
        const auto liveId = live.value("uuid").toString();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", live, 13)));
        QTRY_VERIFY_WITH_TIMEOUT(rowWith(liveId).contains("id"), 2000);
        QCOMPARE(queries->size(), fetchCount); // no page request was issued
        auto list = fixture.controller.mentionsPage().value("rows").toList();
        QCOMPARE(list.first().toMap().value("id").toString(), liveId); // newest first
        QVERIFY(rowWith(liveId).value("unread").toBool()); // upserted into the unread index

        // A read event owns only the dot; the row stays in the All collection
        // (activity-realtime-applier.lib.ts:70-76).
        auto readEvent = mentionMessage(90);
        readEvent["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readEvent, 14)));
        QTRY_VERIFY(!rowWith(liveId).value("unread").toBool());
        QVERIFY(rowWith(liveId).contains("id"));

        // mentioned === false removes the uuid from the unread index
        // (activity-realtime-applier.lib.ts:77-79).
        auto notMention = mentionMessage(1);
        notMention["mentioned"] = false;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", notMention, 15)));
        QTRY_VERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());

        // A batch read event clears exactly the listed dots
        // (activity-realtime-applier.lib.ts:60-66).
        const QJsonObject batch{{"kind", "messages.read"}, {"project_id", projectId},
            {"message_uuids", QJsonArray{mentionMessage(2).value("uuid").toString()}}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read", batch, 16)));
        QTRY_VERIFY(!rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool());

        // Deleting the message drops the live row (activity-realtime-applier.lib.ts:118-124).
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", liveId},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 17)));
        QTRY_VERIFY(!rowWith(liveId).contains("id"));
        QCOMPARE(fixture.controller.mentionsPage().value("rows").toList().size(), 2);

        // topic.read clears the topic's mention dots (activity-realtime-applier.lib.ts:90-96).
        const QJsonObject topicRead{{"kind", "topic.read"},
            {"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"name", "Native topic"}, {"is_done", false}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.read", topicRead, 18)));
        QTRY_VERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());
        QVERIFY(!rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool());
    }

    // Deletion is reconciled across every mention store (issue #56): the
    // fetched collection rows, the live rows and the unread index all drop
    // the uuid immediately, without a refetch, and no later page response
    // can resurrect the deleted card.
    void activityMentionDeletionPrunesFetchedAllRowsWithoutRefetch()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2)}, {3, mentionMessage(3, true)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.mentions = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 3, 4000);
        const auto rowWith = [&](const QString &id) {
            for (const auto &value : fixture.controller.mentionsPage().value("rows").toList())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };

        // Row 2 now lives in both sources: the fetched collection and the
        // live map (message.created re-upserts the same uuid).
        QVERIFY(fixture.controller.applyRealtimeEvent(
            controllerEvent("message.created", mentionMessage(2), 13)));
        const auto id2 = mentionMessage(2).value("uuid").toString();
        const int fetchCount = queries->size();

        // A confirmed deletion drops the fetched row (which is also a live
        // row) from the open All collection without issuing a refetch.
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", id2},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 14)));
        QTRY_VERIFY(!rowWith(id2).contains("id"));
        QCOMPARE(queries->size(), fetchCount); // pruned in place, no refetch
        QVERIFY(rowWith(mentionMessage(1).value("uuid").toString()).contains("id"));
        QVERIFY(rowWith(mentionMessage(3).value("uuid").toString()).contains("id"));

        // Reading a non-deleted message still preserves its All card; the
        // read only clears the dot (activity-realtime-applier.lib.ts:70-76).
        auto readEvent = mentionMessage(1);
        readEvent["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readEvent, 15)));
        QTRY_VERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());
        QVERIFY(rowWith(mentionMessage(1).value("uuid").toString()).contains("id"));

        // The deleted uuid stays tombstoned: the next real refresh (Unread
        // filter) keeps it gone even though the fixture's server still serves it.
        fixture.controller.selectMentionsFilter("unread");
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);
        QVERIFY(!rowWith(id2).contains("id"));
    }

    void activityMentionDeletionPrunesUnreadCollectionInPlace()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2)}, {3, mentionMessage(3, true)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.mentions = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 3, 4000);
        fixture.controller.selectMentionsFilter("unread");
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 2, 4000);
        const int fetchCount = queries->size();

        // The deletion drops the fetched row from the open Unread collection
        // too, without a refetch of that collection.
        const auto id1 = mentionMessage(1).value("uuid").toString();
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", id1},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 13)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 1, 4000);
        QCOMPARE(queries->size(), fetchCount);
    }

    void activityMentionDeletedDuringOutstandingPageCannotBeResurrected()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2, true)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto released = std::make_shared<bool>(false);
        fixture.mentions = [rows, gates, released](const Request &request) {
            const QUrlQuery query(request.target);
            // Hold the first All collection page in flight, frozen at the
            // pre-deletion snapshot that includes row 2.
            if (!*released && query.queryItemValue("mentioned") == "true"
                && query.queryItemValue("page_limit") == "50") {
                Response response{QJsonArray{rows->value(1), rows->value(2)}, 200, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight

        // The deletion of row 2 arrives while that page response is still
        // outstanding.
        const auto id2 = mentionMessage(2).value("uuid").toString();
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", id2},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 13)));
        *released = true;
        for (auto &release : *gates) release();
        gates->clear();

        // The late page may not resurrect the deleted row: only row 1 lands.
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.mentionsPage().value("rows").toList().size() == 1, 4000);
        const auto list = fixture.controller.mentionsPage().value("rows").toList();
        for (const auto &value : list)
            QVERIFY(value.toMap().value("id").toString() != id2);
        QVERIFY(list.first().toMap().value("id").toString()
            == mentionMessage(1).value("uuid").toString());
    }

    void activityMentionIndexFallbackUntilWalkCompletes()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1, true)}, {2, mentionMessage(2)}, {3, mentionMessage(3)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto released = std::make_shared<bool>(false);
        fixture.mentions = [rows, gates, released](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (!*released && query.queryItemValue("page_limit") == "100") {
                Response response{QJsonArray{}, 200, 0, {}}; // authoritative index: nobody is unread
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 3, 4000);
        // Fallback rule: without the index the row's own read flag decides
        // (activity-page.test.tsx:556) - rows 2 and 3 look unread, row 1 does not.
        const auto rowWith = [&](const QString &id) {
            for (const auto &value : fixture.controller.mentionsPage().value("rows").toList())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };
        QVERIFY(rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool());
        QVERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());
        // Once the (empty) index walk completes the index is authoritative and
        // the fallback dot flips off (activity-page.test.tsx:590, 633).
        for (auto &release : *gates) release();
        gates->clear();
        QTRY_VERIFY_WITH_TIMEOUT(
            !rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool(), 4000);
    }

    void activityMentionStaleOwnerSwitchDiscardsPage()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto released = std::make_shared<bool>(false);
        fixture.mentions = [rows, gates, released](const Request &request) {
            const QUrlQuery query(request.target);
            if (!*released && query.hasQueryItem("mentioned")) {
                Response response{QJsonArray{}, 200, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight
        fixture.controller.logout();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.authenticated(), 4000);
        for (auto &release : *gates) release();
        gates->clear();
        *released = true;
        QTest::qWait(250); // give the late responses a chance to misfire
        QVERIFY(!fixture.controller.mentionsVisible());
        QVERIFY(fixture.controller.mentionsPage().value("rows").toList().isEmpty());
    }

    // ---- Activity starred messages page (issue #41, docs/NATIVE_DESKTOP_ACTIVITY_STARRED.md) ----

    static QJsonObject starredMessage(int number)
    {
        const auto id = QStringLiteral("7a7a7a7a-7a7a-4a7a-8a7a-")
            + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
        auto row = message(QStringLiteral("Starred message %1").arg(number), id);
        row.insert("starred", true);
        row.insert("is_own", false);
        row.insert("read", true);
        row.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                         .addSecs(number).toString(Qt::ISODateWithMs));
        return row;
    }

    // Serves the starred=true message page like the Workspace API:
    // page_limit caps the page and the continuation cursor rides the
    // X-Pagination-Marker header (activity-workspace-starred.api.ts:57-71).
    static Response starredPageResponse(const QMap<int, QJsonObject> &rows, const Request &request)
    {
        const auto query = QUrlQuery(request.target);
        const int limit = qMax(1, query.queryItemValue("page_limit").toInt());
        const auto marker = query.queryItemValue("page_marker");
        const int boundary = marker.isEmpty() ? std::numeric_limits<int>::max()
                                             : marker.right(12).toInt(nullptr, 16);
        QJsonArray items;
        bool more = false;
        for (auto it = rows.cend(); it != rows.cbegin();) {
            --it;
            if (it.key() >= boundary)
                continue;
            if (items.size() == limit) {
                more = true;
                break;
            }
            items.append(it.value());
        }
        Response response{items, 200, 0, {}};
        if (more)
            response.headers.insert("X-Pagination-Marker",
                items.last().toObject().value("uuid").toString().toUtf8());
        return response;
    }

    void activityStarredPageIssuesServerFilteredQuery()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, starredMessage(1)}, {2, starredMessage(2)}, {3, starredMessage(3)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return starredPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        QVERIFY(!fixture.controller.starredVisible());
        fixture.controller.showStarred();
        QVERIFY(fixture.controller.starredVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.starredPage().value("loading").toBool(), 4000);

        // The starred query is exactly the reference contract
        // (activity-workspace-starred.api.ts:57-64, activity-page.ui.tsx:44):
        // starred=true, newest-first sort, page_limit=50, no cursor.
        const auto starredQuery = [&] {
            for (const auto &query : *queries)
                if (query.queryItemValue("starred") == "true") return query;
            return QUrlQuery{};
        }();
        QVERIFY(starredQuery.hasQueryItem("starred"));
        QCOMPARE(starredQuery.queryItemValue("page_limit"), QStringLiteral("50"));
        QCOMPARE(starredQuery.queryItemValue("sort_key"), QStringLiteral("created_at"));
        QCOMPARE(starredQuery.queryItemValue("sort_dir"), QStringLiteral("desc"));
        QVERIFY(!starredQuery.hasQueryItem("page_marker"));

        auto page = fixture.controller.starredPage();
        const auto list = page.value("rows").toList();
        QCOMPARE(list.size(), 3);
        // Newest first (activity-page.ui.tsx:239 sortUniqueActivityMessages reverse).
        QCOMPARE(list.first().toMap().value("id").toString(),
            starredMessage(3).value("uuid").toString());
        QCOMPARE(list.last().toMap().value("id").toString(),
            starredMessage(1).value("uuid").toString());
        // Row enrichment for the measured card: author, time and the
        // markdown flag decide the sender line, the meta line and whether
        // the forward hover action is enabled.
        const auto row = list.first().toMap();
        QVERIFY(!row.value("author").toString().isEmpty());
        QVERIFY(!row.value("time").toString().isEmpty());
        QVERIFY(row.value("preview").toString().contains("Starred message 3"));
        QVERIFY(row.value("markdown").toBool());
        QVERIFY(!page.value("canLoadMore").toBool());
        // The pages stay mutually exclusive (issue #54 sidebar rule).
        QVERIFY(!fixture.controller.mentionsVisible());
        QVERIFY(!fixture.controller.draftsVisible());
    }

    void activityStarredPaginationFollowsMarkerNewestFirst()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        auto rows = std::make_shared<QMap<int, QJsonObject>>();
        for (int n = 1; n <= 60; ++n)
            rows->insert(n, starredMessage(n));
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return starredPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showStarred();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.starredPage().value("rows").toList().size() == 50, 4000);
        QVERIFY(fixture.controller.starredPage().value("canLoadMore").toBool());

        fixture.controller.loadMoreStarred();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.starredPage().value("rows").toList().size() == 60, 4000);
        QVector<QUrlQuery> starredQueries;
        for (const auto &query : *queries)
            if (query.queryItemValue("starred") == "true")
                starredQueries.append(query);
        QCOMPARE(starredQueries.size(), 2);
        // The continuation rode X-Pagination-Marker back into page_marker and
        // names the oldest row of the first page (key 11 = ...00000000000b).
        QVERIFY(starredQueries.at(1).hasQueryItem("page_marker"));
        QCOMPARE(starredQueries.at(1).queryItemValue("page_marker"),
            starredMessage(11).value("uuid").toString());
        const auto list = fixture.controller.starredPage().value("rows").toList();
        QCOMPARE(list.size(), 60);
        QSet<QString> ids;
        for (const auto &value : list) ids.insert(value.toMap().value("id").toString());
        QCOMPARE(ids.size(), 60); // dedupe by uuid (activity-page.ui.tsx:95-98)
        QCOMPARE(list.first().toMap().value("id").toString(), starredMessage(60).value("uuid").toString());
        QCOMPARE(list.last().toMap().value("id").toString(), starredMessage(1).value("uuid").toString());
        QVERIFY(!fixture.controller.starredPage().value("canLoadMore").toBool());

        // Reopening refreshes from the first page - no cursor leak
        // (activity-page.ui.tsx:283-297 collectionKey reset).
        const auto totalBefore = queries->size();
        fixture.controller.closeStarred();
        QVERIFY(!fixture.controller.starredVisible());
        fixture.controller.showStarred();
        QTRY_VERIFY_WITH_TIMEOUT(queries->size() > totalBefore, 4000);
        for (auto it = queries->cbegin() + totalBefore; it != queries->cend(); ++it)
            if (it->queryItemValue("starred") == "true")
                QVERIFY(!it->hasQueryItem("page_marker"));
    }

    void activityStarredErrorStatesAndRetry()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        auto rows = std::make_shared<QMap<int, QJsonObject>>();
        for (int n = 1; n <= 60; ++n)
            rows->insert(n, starredMessage(n));
        auto failFirst = std::make_shared<bool>(false);
        auto failMarked = std::make_shared<bool>(false);
        fixture.messages = [rows, failFirst, failMarked](const Request &request) {
            const QUrlQuery query(request.target);
            if (query.queryItemValue("starred") == "true") {
                if (*failFirst)
                    return Response{QJsonArray{}, 500, 0, {}};
                if (*failMarked && query.hasQueryItem("page_marker"))
                    return Response{QJsonArray{}, 500, 0, {}};
            }
            return starredPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        // Initial load error: the list stays empty and owns the error flag
        // (activity-page.ui.tsx:580-592 error box state).
        *failFirst = true;
        fixture.controller.showStarred();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.starredPage().value("error").toBool(), 4000);
        QVERIFY(fixture.controller.starredPage().value("rows").toList().isEmpty());
        *failFirst = false;
        fixture.controller.retryStarred();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.starredPage().value("loading").toBool(), 4000);
        QVERIFY(!fixture.controller.starredPage().value("error").toBool());
        QCOMPARE(fixture.controller.starredPage().value("rows").toList().size(), 50);
        QVERIFY(fixture.controller.starredPage().value("canLoadMore").toBool());

        // Pagination error keeps the fetched rows and the retry link refires
        // the older page with the same cursor (activity-page.ui.tsx:607-615).
        *failMarked = true;
        fixture.controller.loadMoreStarred();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.starredPage().value("paginationError").toBool(), 4000);
        QCOMPARE(fixture.controller.starredPage().value("rows").toList().size(), 50);
        *failMarked = false;
        fixture.controller.retryStarred();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.starredPage().value("rows").toList().size() == 60, 4000);
        QVERIFY(!fixture.controller.starredPage().value("paginationError").toBool());
    }

    void activityStarredStaleOwnerSwitchDiscardsPage()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(
            QMap<int, QJsonObject>{{1, starredMessage(1)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto released = std::make_shared<bool>(false);
        fixture.messages = [rows, gates, released](const Request &request) {
            const QUrlQuery query(request.target);
            if (!*released && query.queryItemValue("starred") == "true") {
                Response response{QJsonArray{}, 200, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return starredPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showStarred();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight
        fixture.controller.logout();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.authenticated(), 4000);
        for (auto &release : *gates) release();
        gates->clear();
        *released = true;
        QTest::qWait(250); // give the late response a chance to misfire
        QVERIFY(!fixture.controller.starredVisible());
        QVERIFY(fixture.controller.starredPage().value("rows").toList().isEmpty());
    }

    // exordy review #41: reopening the page while a request is in flight must
    // not silently drop the mount refresh (activity-page.ui.tsx:283-297
    // refreshes from the first page on every mount): the refresh is queued and
    // re-issued once the in-flight response lands.
    void activityStarredReopenDuringInflightReissuesFirstPage()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        auto rows = std::make_shared<QMap<int, QJsonObject>>(
            QMap<int, QJsonObject>{{1, starredMessage(1)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto holdFirst = std::make_shared<bool>(false);
        fixture.messages = [rows, queries, gates, holdFirst](const Request &request) {
            const QUrlQuery query(request.target);
            if (query.queryItemValue("starred") == "true") {
                queries->append(query);
                if (*holdFirst) {
                    *holdFirst = false;
                    Response response{starredPageResponse(*rows, request).data, 200, 0, {}};
                    response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                    return response;
                }
            }
            return starredPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        *holdFirst = true;
        fixture.controller.showStarred();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight
        // Re-open while the first page is still in flight: the refresh demand
        // is recorded instead of dropped by the in-flight guard.
        fixture.controller.closeStarred();
        fixture.controller.showStarred();
        QCOMPARE(queries->size(), 1); // the guard still never runs two fetches
        // A star lands while the first page is in flight: only a re-issued
        // first-page fetch can surface it on the reopened page.
        rows->insert(2, starredMessage(2));
        for (auto &release : *gates) release();
        gates->clear();
        // The queued refresh re-issues the first-page fetch once the
        // in-flight response lands: without a cursor, with current rows.
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.starredPage().value("rows").toList().size() == 2, 4000);
        int starredRequests = 0;
        for (const auto &query : *queries)
            if (query.queryItemValue("starred") == "true")
                ++starredRequests;
        QCOMPARE(starredRequests, 2);
        QVERIFY(!queries->last().hasQueryItem("page_marker"));
    }

    // ---- Favorites self-chat (issue #70, docs/NATIVE_DESKTOP_FAVORITES_SELF_CHAT.md) ----
    //
    // Reference contract (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af):
    // workspace-self-chat.hook.ts:48-76 (ensure direct stream with self,
    // "Personal notes", one in-flight request per runtime generation),
    // messenger-self-chat.lib.ts:8-18 (isPrivate && directUserUuid === user),
    // messenger-sidebar.lib.ts:560,592,616,656 and sidebar-shell.ui.tsx:124
    // (self stream filtered from every ordinary chat projection).

    void favoritesEnsureCreatesSelfStreamOnceAndReuses()
    {
        Fixture fixture;
        auto posts = std::make_shared<int>(0);
        auto postBodies = std::make_shared<QVector<QJsonObject>>();
        auto postStatus = std::make_shared<int>(201);
        auto selfStream = selfChatStreamDto();
        const auto fallbackTopics = fixture.topics;
        fixture.topics = [this, fallbackTopics](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == selfStreamId)
                return Response{QJsonArray{selfDefaultTopicDto()}};
            if (!fallbackTopics) return Response{QJsonArray{QJsonObject{{"uuid", topicId},
                {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}}}};
            return fallbackTopics(request);
        };
        fixture.streams = [posts, postBodies, postStatus, selfStream](const Request &request) {
            if (request.method == "POST") {
                ++*posts;
                postBodies->append(request.body);
                return Response{selfStream, *postStatus, 0, {}};
            }
            QJsonArray rows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}};
            if (*posts > 0) rows.append(selfStream);
            return Response{rows};
        };
        QVERIFY(fixture.start());
        QVERIFY(!fixture.controller.favoritesVisible());
        fixture.controller.showFavorites();
        QVERIFY(fixture.controller.favoritesVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("ready"), 4000);
        // The reference create body (messenger-create-chat-actions.lib.ts:202-217
        // through workspace-self-chat.hook.ts:53-60): "Personal notes", empty
        // description, source_name native, direct_user_uuid = the session user.
        QCOMPARE(*posts, 1);
        QCOMPARE(postBodies->first().value("name").toString(), QStringLiteral("Personal notes"));
        QCOMPARE(postBodies->first().value("description").toString(), QString{});
        QCOMPARE(postBodies->first().value("source_name").toString(), QStringLiteral("native"));
        QCOMPARE(postBodies->first().value("direct_user_uuid").toString(), userId);
        // ready lands on the self stream's default topic (hook ready route:
        // {streamUuid, topicUuid}).
        QCOMPARE(fixture.controller.selectedStream(), selfStreamId);
        QCOMPARE(fixture.controller.selectedTopic(), selfDefaultTopic);
        // Repeated calls reuse the catalog entry: no second create request,
        // still ready on the same route.
        fixture.controller.closeFavorites();
        QVERIFY(!fixture.controller.favoritesVisible());
        fixture.controller.showFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("ready"), 4000);
        QCOMPARE(*posts, 1);
        QCOMPARE(fixture.controller.selectedStream(), selfStreamId);
        QCOMPARE(fixture.controller.selectedTopic(), selfDefaultTopic);
    }

    void favoritesEnsureDeduplicatesConcurrentCallsPerGeneration()
    {
        Fixture fixture;
        auto posts = std::make_shared<int>(0);
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto selfStream = selfChatStreamDto();
        const auto fallbackTopics = fixture.topics;
        fixture.topics = [this, fallbackTopics](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == selfStreamId)
                return Response{QJsonArray{selfDefaultTopicDto()}};
            if (!fallbackTopics) return Response{QJsonArray{QJsonObject{{"uuid", topicId},
                {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}}}};
            return fallbackTopics(request);
        };
        fixture.streams = [posts, gates, selfStream](const Request &request) {
            if (request.method == "POST") {
                ++*posts;
                Response response{selfStream, 201, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            QJsonArray rows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}};
            return Response{rows};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the create is in flight
        // Repeated calls while in flight reuse the one request per generation
        // (ensureRequestsByRuntimeKey, workspace-self-chat.hook.ts:48-56).
        fixture.controller.showFavorites();
        fixture.controller.retryFavorites();
        QCOMPARE(*posts, 1);
        QVERIFY(fixture.controller.favoritesStatus() == QStringLiteral("loading"));
        for (auto &release : *gates) release();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("ready"), 4000);
        QCOMPARE(*posts, 1);
    }

    // exordy review 4614001531: a create still in flight when the user
    // navigates away must still land in the catalog, but it must NOT re-open
    // the Favorites route the user left (the callback's openSelfStream used
    // to re-select the self stream and re-set favoritesVisible).
    void favoritesEnsureInFlightNavigateAwayKeepsCatalogButStaysClosed()
    {
        Fixture fixture;
        const QString otherStreamId = QStringLiteral("e5e5e5e5-e5e5-4e5e-8e5e-000000000005");
        auto posts = std::make_shared<int>(0);
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto selfStream = selfChatStreamDto();
        const auto fallbackTopics = fixture.topics;
        fixture.topics = [this, fallbackTopics](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == selfStreamId)
                return Response{QJsonArray{selfDefaultTopicDto()}};
            if (!fallbackTopics) return Response{QJsonArray{QJsonObject{{"uuid", topicId},
                {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}}}};
            return fallbackTopics(request);
        };
        fixture.streams = [posts, gates, selfStream, otherStreamId](const Request &request) {
            if (request.method == "POST") {
                ++*posts;
                Response response{selfStream, 201, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            QJsonArray rows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}},
                QJsonObject{{"uuid", otherStreamId}, {"project_id", projectId},
                    {"name", "Second channel"}, {"encryption", false}}};
            return Response{rows};
        };
        QVERIFY(fixture.start({}, 2));
        fixture.controller.showFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the create is in flight
        // The user navigates to a different ordinary chat while the create is
        // in flight (selectStream leaves the favorites route, mirroring route
        // unmount).
        fixture.controller.selectStream(otherStreamId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedStream() == otherStreamId, 2000);
        QVERIFY(!fixture.controller.favoritesVisible());
        // The create response lands: the stream must reach the catalog ...
        auto catalogHasSelf = [&fixture] {
            for (const auto &value : fixture.controller.streams())
                if (value.toMap().value("id").toString() == selfStreamId) return true;
            return false;
        };
        for (auto &release : *gates) release();
        QTRY_VERIFY_WITH_TIMEOUT((catalogHasSelf()), 2000);
        // ... and the same callback must not re-open Favorites or steal the
        // selection the user made after leaving.
        QVERIFY(!fixture.controller.favoritesVisible());
        QCOMPARE(fixture.controller.selectedStream(), otherStreamId);
        // A later reopen uses the catalog entry: no second create, lands ready.
        fixture.controller.showFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("ready"), 4000);
        QCOMPARE(*posts, 1);
        QCOMPARE(fixture.controller.selectedStream(), selfStreamId);
    }

    // exordy review 4614001539: leaving Favorites clears its status; reopening
    // while an earlier ensure is still in flight hits the per-generation dedup
    // early-return and must still show the loading state, not an empty page.
    void favoritesReopenDuringInflightEnsureRestoresLoading()
    {
        Fixture fixture;
        auto posts = std::make_shared<int>(0);
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto selfStream = selfChatStreamDto();
        const auto fallbackTopics = fixture.topics;
        fixture.topics = [this, fallbackTopics](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == selfStreamId)
                return Response{QJsonArray{selfDefaultTopicDto()}};
            if (!fallbackTopics) return Response{QJsonArray{QJsonObject{{"uuid", topicId},
                {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}}}};
            return fallbackTopics(request);
        };
        fixture.streams = [posts, gates, selfStream](const Request &request) {
            if (request.method == "POST") {
                ++*posts;
                Response response{selfStream, 201, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            QJsonArray rows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}};
            return Response{rows};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the create is in flight
        // Leaving the route clears its view state (route unmount).
        fixture.controller.closeFavorites();
        QVERIFY(fixture.controller.favoritesStatus().isEmpty());
        // Reopening while the earlier ensure is still in flight reuses the one
        // request per generation ...
        fixture.controller.showFavorites();
        QVERIFY(fixture.controller.favoritesVisible());
        QCOMPARE(*posts, 1);
        // ... and must show the spinner again (the dedup early-return used to
        // leave the status cleared, so the page showed neither).
        QVERIFY(fixture.controller.favoritesStatus() == QStringLiteral("loading"));
        // The in-flight create then completes the reopened page normally.
        for (auto &release : *gates) release();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("ready"), 4000);
        QCOMPARE(fixture.controller.selectedStream(), selfStreamId);
        QCOMPARE(*posts, 1);
    }

    void favoritesEnsureFailureSetsErrorAndRetryRecovers()
    {
        Fixture fixture;
        auto posts = std::make_shared<int>(0);
        auto postStatus = std::make_shared<int>(500);
        auto selfStream = selfChatStreamDto();
        const auto fallbackTopics = fixture.topics;
        fixture.topics = [this, fallbackTopics](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == selfStreamId)
                return Response{QJsonArray{selfDefaultTopicDto()}};
            if (!fallbackTopics) return Response{QJsonArray{QJsonObject{{"uuid", topicId},
                {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}}}};
            return fallbackTopics(request);
        };
        fixture.streams = [posts, postStatus, selfStream](const Request &request) {
            if (request.method == "POST") {
                ++*posts;
                if (*postStatus != 201) return Response{QJsonObject{{"detail", "nope"}}, *postStatus, 0, {}};
                return Response{selfStream, 201, 0, {}};
            }
            QJsonArray rows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}};
            return Response{rows};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("error"), 4000);
        // The reference error card copy is favorites.loadError regardless of
        // the transport detail (chat-page.ui.tsx:56-65).
        QVERIFY(!fixture.controller.favoritesError().isEmpty());
        QVERIFY(fixture.controller.favoritesVisible());
        QCOMPARE(*posts, 1);
        // Retry starts a fresh request (the settled entry is dropped,
        // workspace-self-chat.hook.ts:63-68).
        *postStatus = 201;
        fixture.controller.retryFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("ready"), 4000);
        QCOMPARE(*posts, 2);
        QCOMPARE(fixture.controller.selectedStream(), selfStreamId);
        QCOMPARE(fixture.controller.selectedTopic(), selfDefaultTopic);
    }

    void favoritesSelfStreamIsHiddenFromOrdinaryLists()
    {
        Fixture fixture;
        auto posts = std::make_shared<int>(0);
        auto selfStream = selfChatStreamDto();
        const auto fallbackTopics = fixture.topics;
        fixture.topics = [this, fallbackTopics](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == selfStreamId)
                return Response{QJsonArray{selfDefaultTopicDto()}};
            if (!fallbackTopics) return Response{QJsonArray{QJsonObject{{"uuid", topicId},
                {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}}}};
            return fallbackTopics(request);
        };
        fixture.streams = [posts, selfStream](const Request &request) {
            if (request.method == "POST") ++*posts;
            QJsonArray rows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}, selfStream};
            return Response{rows};
        };
        // One ordinary row is visible; the self stream never enters any
        // sidebar/folder projection (messenger-sidebar.lib.ts:560,592,616,656).
        QVERIFY(fixture.start({}, 2));
        const auto ids = [](const QVariantList &rows) {
            QStringList result;
            for (const auto &value : rows) result.append(value.toMap().value("id").toString());
            return result;
        };
        // streams() keeps the lookup row (header/encryption lookups resolve it);
        // only the sidebar and folder projections drop it.
        auto catalogIds = ids(fixture.controller.streams());
        catalogIds.sort();
        const QStringList expectedCatalog{streamId, selfStreamId}; // sorted by id (3333... < d0d0...)
        QCOMPARE(catalogIds, expectedCatalog);
        QCOMPARE(ids(fixture.controller.sidebarStreams()), QStringList{streamId});
        // A Personal notes stream created by any client still opens through
        // Favorites without any create request.
        fixture.controller.showFavorites();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.favoritesStatus() == QStringLiteral("ready"), 4000);
        QCOMPARE(*posts, 0);
        QCOMPARE(fixture.controller.selectedStream(), selfStreamId);
        QCOMPARE(ids(fixture.controller.sidebarStreams()), QStringList{streamId});
    }

    // ---- Issue #101: activity panel counts, badge cap, per-session state ----

    void activityBadgeLabelCapsUnreadCountsAtDisplayLimit()
    {
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        // count-format.lib.ts formatCount(count, UNREAD_COUNT_DISPLAY_LIMIT=1000):
        // strictly above 1000 compacts into whole thousands (1001 -> "1k+").
        QCOMPARE(controller.activityBadgeLabel(0), QStringLiteral("0"));
        QCOMPARE(controller.activityBadgeLabel(999), QStringLiteral("999"));
        QCOMPARE(controller.activityBadgeLabel(1000), QStringLiteral("1000"));
        QCOMPARE(controller.activityBadgeLabel(1001), QStringLiteral("1k+"));
        QCOMPARE(controller.activityBadgeLabel(2999), QStringLiteral("2k+"));
        QCOMPARE(controller.activityBadgeLabel(3000), QStringLiteral("3k+"));
    }

    void activityInboxCountFollowsAllFolderUnread()
    {
        FolderRailFixture f;
        // Not authenticated yet: the reference count is null (-1 natively),
        // so no badge may render (selectMessengerSidebarActivityCounts).
        QCOMPARE(f.controller.activityInboxCount(), -1);
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 7, 0),
            folderRailFolder(channelsFolder, "Channels", "channels", 2, 0)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.activityInboxCount(), 7, 4000);
    }

    void activityMentionsCountStaysNullUntilIndexLoaded()
    {
        FolderRailFixture f;
        QCOMPARE(f.controller.activityMentionsCount(), -1);
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.activityMentionsCount(), 0, 3000);
        QVERIFY(!f.controller.mentionsVisible());
    }

    void activityOpenStatePersistsPerWorkspaceSession()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("activity.ini");
        NativePreferences preferences(path);
        FolderRailFixture f;
        f.controller.usePreferences(&preferences);
        QVERIFY(f.start());
        // Default closed (sidebar-config.model.ts DEFAULT_CONFIG).
        QVERIFY(!f.controller.activityOpen());
        QSignalSpy opened(&f.controller, &NativeController::activityOpenChanged);
        f.controller.setActivityOpen(true);
        QCOMPARE(opened.count(), 1);
        f.controller.setActivityOpen(true);
        QCOMPARE(opened.count(), 1); // unchanged state emits nothing
        const auto owner = conversationPreferenceOwner(f.server.origin());
        NativePreferences reopened(path);
        QVERIFY(reopened.loadActivityOpen(owner));
        // Another workspace session keeps its own (closed) state.
        QVERIFY(!reopened.loadActivityOpen(
            conversationPreferenceOwner(QUrl::fromUserInput("https://elsewhere.example"))));
        // A fresh session for the same workspace scope (same origin, user and
        // project => identical creation owner key) restores the open panel.
        ApiClient otherApi(nullptr, true);
        Session otherSession(&otherApi);
        NativeController otherController(&otherApi, &otherSession);
        otherController.usePreferences(&reopened);
        otherController.login(f.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(otherController.projects().size(), 1);
        otherController.openProject(projectId);
        QTRY_VERIFY(otherController.activityOpen());
        // Collapsing overwrites the persisted state with closed.
        otherController.setActivityOpen(false);
        NativePreferences closed(path);
        QVERIFY(!closed.loadActivityOpen(owner));
    }
};

QTEST_MAIN(ControllerActivityTest)
#include "native-controller-activity-test.moc"
