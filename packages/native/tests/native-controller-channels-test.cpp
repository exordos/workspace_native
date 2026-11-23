#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"

// ---- Issue #635: topic-summary settings mock scene (docs/topic-summary-settings.md) ----
static const QString summaryEndpointUuid =
    QStringLiteral("77770000-1111-4222-8333-444455556666");

struct TopicSummaryScene {
    Fixture fixture;
    QJsonArray permissions;
    QString role = QStringLiteral("owner");
    bool ownStream = true;
    int gatesGets = 0;
    int gatesPuts = 0;
    int endpointGets = 0;
    QList<QJsonObject> endpointPosts;
    QList<QJsonObject> endpointPuts;
    QStringList endpointDeletes;
    QJsonObject gatesResponse{{"project_id", projectId}, {"global_enabled", true},
        {"project_enabled", false}};
    QJsonArray endpointsResponse;
    int gatesStatus = 200;
    int endpointsStatus = 200;
    // Issue #635 topic-tab helpers: topicSummaryConfig merges into every served
    // topic row DTO (summary_enabled / summary_system_prompt / summary_reasoning_effort).
    QJsonObject topicSummaryConfig;
    QJsonObject topicRow(const QString &id) const
    {
        auto row = QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"name", id == secondTopicId ? "Second topic" : "Native topic"},
            {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0},
            {"last_message_uuid", QJsonValue::Null}};
        for (const auto key : topicSummaryConfig.keys()) row.insert(key, topicSummaryConfig.value(key));
        return row;
    }
    void serveTopicRows(Fixture &fixture) const
    {
        fixture.topics = [this](const Request &) {
            return Response{QJsonArray{topicRow(topicId), topicRow(secondTopicId)}};
        };
    }
    TopicSummaryScene()
    {
        fixture.streams = [this](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false},
                {"user_uuid", ownStream ? userId : QStringLiteral("99990000-1111-4222-8333-444455556666")},
                {"role", role}}}};
        };
        // The stream-detail route feeds the same catalog row, so it must carry
        // the ownership fields as well (fixture default shape plus role/user_uuid).
        fixture.counters = [this](const Request &request) {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", QJsonValue::Null},
                    {"user_uuid", ownStream ? userId : QStringLiteral("99990000-1111-4222-8333-444455556666")},
                    {"role", role}}};
            return Response{QJsonObject{{"uuid", path.endsWith(secondTopicId) ? secondTopicId : topicId},
                {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"},
                {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", QJsonValue::Null}}};
        };
        fixture.authentication = [this](const Request &request) {
            const auto path = request.target.path();
            if (path.contains("topic_summary_settings")) {
                if (request.method == "PUT") { ++gatesPuts; lastGatesPut = request.body; }
                else ++gatesGets;
                return Response{gatesResponse, gatesStatus};
            }
            if (path.contains("topic_summary_endpoints")) {
                if (request.method == "POST") { endpointPosts.append(request.body); return Response{QJsonObject{}, 200}; }
                if (request.method == "PUT") { endpointPuts.append(request.body); return Response{QJsonObject{}, 200}; }
                if (request.method == "DELETE") { endpointDeletes.append(path.section('/', -1)); return Response{QJsonObject{}, 200}; }
                ++endpointGets;
                return Response{endpointsResponse, endpointsStatus};
            }
            if (path.endsWith("/actions/introspect")) {
                auto json = sessionResponse(request).data.toObject();
                json["permissions"] = permissions;
                return Response{json};
            }
            return sessionResponse(request);
        };
    }
    QJsonObject lastGatesPut;
};

static bool isUuidText(const QString &value)
{
    static const QRegularExpression pattern{
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")};
    return pattern.match(value).hasMatch();
}

static QJsonObject summaryEndpointRow(const QString &uuid = summaryEndpointUuid)
{
    return {{"uuid", uuid}, {"name", "Prod"}, {"base_url", "https://llm.example/v1"},
        {"model", "gpt-x"}, {"enabled", true}, {"priority", 100}, {"supports_vision", false},
        {"supports_reasoning", false}, {"temperature", 0.2}, {"max_output_tokens", 512},
        {"top_p", 1}, {"presence_penalty", 0}, {"frequency_penalty", 0},
        {"credential_present", true}, {"failure_count", 0}};
}

class ControllerChannelsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void channelCreationPublicGateAndInitialPhase()
    {
        ChannelCreationFixture test; auto &controller = test.fixture.controller;
        QVERIFY(!controller.canCreateChannel()); QVERIFY(controller.channelCreationContextKey().isEmpty());
        QCOMPARE(controller.channelCreationPhase(), QString("idle")); QVERIFY(controller.channelCreationForm().isEmpty());
        QVERIFY(test.fixture.start()); QTRY_VERIFY(controller.canCreateChannel());
        QVERIFY(!controller.channelCreationContextKey().isEmpty());
        QVERIFY(!controller.canCheckChannelCreation()); QVERIFY(!controller.canRetryChannelMembers());
    }

    void channelCreationPublishesVerifiedAggregateAndKeepsMembers_data()
    {
        QTest::addColumn<bool>("members"); QTest::newRow("empty") << false; QTest::newRow("selected") << true;
    }
    void channelCreationPublishesVerifiedAggregateAndKeepsMembers()
    {
        QFETCH(bool, members); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy complete(&controller, &NativeController::channelCreated);
        const auto users = test.usersGets, streams = test.streamGets, history = test.fixture.totalHistoryGets();
        controller.createChannel("  Created channel  ", "  Description  ", true, true,
            members ? QVariantList{channelMemberA, channelMemberB, channelMemberA, userId} : QVariantList{});
        QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY2(controller.channelCreationPhase() == "ready", qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(complete.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 1); QCOMPARE(test.topicGets, 1);
        QCOMPARE(test.streamGets - streams, 1); QCOMPARE(test.usersGets, users);
        QCOMPARE(test.memberPosts, members ? 1 : 0); QCOMPARE(test.bindingGets, 0);
        QCOMPARE(controller.channelCreationPhase(), QString("ready")); QVERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.canCreateChannel()); QCOMPARE(controller.selectedStream(), test.created.value("uuid").toString());
        QVERIFY(controller.selectedTopic().isEmpty()); QCOMPARE(controller.conversationKind(), QString("stream"));
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach()); QCOMPARE(controller.topics().size(), 1);
        QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), controller.selectedStream());
        QCOMPARE(test.streamBodies.first(), QJsonObject({{"uuid", controller.selectedStream()}, {"name", "Created channel"},
            {"description", "Description"}, {"source_name", "native"}, {"invite_only", true}, {"announce", true},
            {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null}}));
        if (members) QCOMPARE(test.memberBodies.first(), QJsonObject({{"member", QJsonArray{channelMemberA, channelMemberB}}}));
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1); // Only the new aggregate route.
    }

    void channelDirectoryIsVerifiedDeduplicatedAndDoesNotChangeFormLifetime()
    {
        ChannelCreationFixture test;
        const auto source = test.fixture.users;
        test.fixture.users = [source](const Request &request) {
            auto result = source(request); auto rows = result.data.toArray(); rows.append(rows.at(1)); result.data = rows; return result;
        };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QVERIFY(controller.channelMembersVerified()); QCOMPARE(controller.channelMemberChoices().size(), 2);
        const auto alice = controller.channelMemberChoices().first().toMap();
        QCOMPARE(alice, QVariantMap({{"id", channelMemberA}, {"displayName", "Alice Adams"},
            {"username", "alice"}, {"email", "alice@example.invalid"}}));
        const auto context = controller.channelCreationContextKey();
        auto stream = creationChannel(streamId, "Renamed by somebody"); stream["unread_count"] = 12;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.read", stream)));
        auto topic = creationTopic(topicId, streamId, "Renamed topic"); topic["unread_count"] = 9;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.read", topic)));
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", channelMemberA},
            {"username", "alice"}, {"first_name", "Alice"}, {"last_name", "Updated"}, {"email", "new@example.invalid"}})));
        QCOMPARE(controller.channelCreationContextKey(), context); QVERIFY(controller.channelMembersVerified());
        QCOMPARE(controller.channelMemberChoices().first().toMap().value("displayName").toString(), "Alice Updated");
        QCOMPARE(controller.channelMemberChoices().last().toMap().value("displayName").toString(), "bob");
        controller.selectStream({}); QVERIFY(controller.channelCreationContextKey() != context);
        controller.logout(); QVERIFY(controller.channelMemberChoices().isEmpty()); QVERIFY(!controller.channelMembersVerified());
    }

    void channelWithoutMembersDoesNotWaitForTheDirectory()
    {
        ChannelCreationFixture test; test.fixture.users = [](const Request &) { return Response{QJsonObject{}, 503}; };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QVERIFY(!controller.channelMembersVerified());
        QVERIFY(controller.canCreateChannel()); const auto users = test.usersGets;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("No members", "", false, false, {}); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.memberPosts, 0); QCOMPARE(test.usersGets, users);
    }

    void channelCreationValidatesUnicodeAndSelectedIdsBeforeAnyPost_data()
    {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("description");
        QTest::addColumn<QVariantList>("members"); QTest::addColumn<bool>("allowed");
        const auto astral = QString::fromUcs4(U"🦊");
        QTest::newRow("blank-ecma") << QString("\ufeff\u00a0 \t") << QString{} << QVariantList{} << false;
        QTest::newRow("name255") << astral.repeated(255) << QString{} << QVariantList{} << true;
        QTest::newRow("name256") << astral.repeated(256) << QString{} << QVariantList{} << false;
        QTest::newRow("description10000") << QString("valid") << astral.repeated(10000) << QVariantList{} << true;
        QTest::newRow("description10001") << QString("valid") << astral.repeated(10001) << QVariantList{} << false;
        QTest::newRow("unpaired-surrogate") << QString(QChar(0xd800)) << QString{} << QVariantList{} << false;
        QTest::newRow("bad-description-surrogate") << QString("valid") << QString(QChar(0xdc00)) << QVariantList{} << false;
        QTest::newRow("invalid-id") << QString("valid") << QString{} << QVariantList{"not-a-uuid"} << false;
        QTest::newRow("coerced-id") << QString("valid") << QString{} << QVariantList{42} << false;
        QTest::newRow("foreign-id") << QString("valid") << QString{} << QVariantList{returnedId} << false;
        QTest::newRow("only-self") << QString("valid") << QString{} << QVariantList{userId} << true;
    }
    void channelCreationValidatesUnicodeAndSelectedIdsBeforeAnyPost()
    {
        QFETCH(QString, name); QFETCH(QString, description); QFETCH(QVariantList, members); QFETCH(bool, allowed);
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel(name, description, false, false, members);
        if (allowed) { QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); }
        else {
            QVERIFY(!controller.channelCreationError().isEmpty()); QCOMPARE(controller.channelCreationPhase(), "idle");
            QVERIFY(controller.channelCreationForm().value("attemptId").toString().isEmpty());
            QCOMPARE(test.posts, 0); QCOMPARE(test.memberPosts, 0); QCOMPARE(done.size(), 0);
        }
    }

    void channelCreationCanonicalIdentityMustMatch_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value"); QTest::addColumn<bool>("post");
        for (const bool post : {true, false}) {
            const auto prefix = post ? "post-" : "get-";
            const QList<QPair<QString, QJsonValue>> invalid{{"uuid", returnedId}, {"project_id", returnedId},
                {"owner", returnedId}, {"user_uuid", returnedId}, {"role", "member"}, {"role", "unknown-role"},
                {"source_name", "zulip"}, {"provider_uuid", returnedId}, {"provider_identity", "provider-value"},
                {"private", true}, {"direct_user_uuid", returnedId}, {"encryption", true},
                {"current_encryption_key_uuid", returnedId}, {"is_archived", true}, {"default_topic_uuid", "bad"},
                {"unread_count", -1}, {"active_unread_count", "0"}, {"notification_mode", "unknown"},
                {"created_at", "bad"}, {"updated_at", QJsonValue::Null}, {"invite_only", "false"}};
            int n = 0;
            for (const auto &entry : invalid)
                QTest::newRow(qPrintable(QString(prefix) + entry.first + QString::number(n++))) << entry.first << entry.second << post;
        }
    }
    void channelCreationCanonicalIdentityMustMatch()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value); QFETCH(bool, post);
        ChannelCreationFixture test; QVERIFY(test.fixture.start());
        const auto alter = [=](QJsonObject &row) { row[field] = value; };
        if (post) test.alterPost = alter; else test.alterGet = alter;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Frozen request", "", false, false, {});
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        QCOMPARE(test.gets, post ? 0 : 1); QVERIFY(controller.canCheckChannelCreation());
        test.alterPost = {}; test.alterGet = {}; controller.checkChannelCreation();
        QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
    }

    void channelCreationAmbiguousPostRecoveryNeverAddsMembersImplicitly()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = 503;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Stable attempt", "Frozen description", true, false, {channelMemberA, channelMemberB});
        QTRY_VERIFY(controller.channelCreationUncertain()); const auto original = controller.channelCreationForm();
        QVERIFY(!original.value("attemptId").toString().isEmpty()); QVERIFY(original.value("confirmedStreamId").toString().isEmpty());
        controller.createChannel("Different request", "", false, true, {}); QCOMPARE(test.posts, 1);
        test.getStatus = 404; controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        test.getStatus = 200; controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); QCOMPARE(test.bindingGets, 1); QCOMPARE(done.size(), 0);
        QVERIFY(controller.canRetryChannelMembers()); QVERIFY(!controller.canCreateChannel()); QVERIFY(!controller.canCheckChannelCreation());
        QCOMPARE(controller.channelCreationForm().value("memberUuids"), original.value("memberUuids"));
        QCOMPARE(controller.channelCreationForm().value("attemptId"), original.value("attemptId"));
        QVERIFY(!controller.channelCreationForm().value("membersAttempted").toBool());
        controller.retryChannelMembers(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(test.bindingGets, 2);
        QCOMPARE(test.memberBodies.first(), QJsonObject({{"member", QJsonArray{channelMemberA, channelMemberB}}}));
    }

    // ---- Issue #234: encrypted channel creation (docs/NATIVE_DESKTOP_ENCRYPTED_CHANNEL_CREATION.md) ----

    void channelMemberRecoveryPreservesTheConfirmedStreamAndFrozenSet_data()
    {
        QTest::addColumn<QString>("outcome");
        for (const auto *outcome : {"terminal", "committed-503", "partial-503", "malformed-200", "promoted-existing"})
            QTest::newRow(outcome) << QString(outcome);
    }
    void channelMemberRecoveryPreservesTheConfirmedStreamAndFrozenSet()
    {
        QFETCH(QString, outcome); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        if (outcome == "terminal" || outcome == "promoted-existing") { test.addStatus = 403; test.applyMembers = false; }
        else if (outcome != "malformed-200") test.addStatus = 503;
        else test.alterBindings = [](QJsonArray &rows) { rows.removeLast(); };
        if (outcome == "partial-503") test.applyMembers = false;
        controller.createChannel("Member recovery", "", false, false, {channelMemberA, channelMemberB});
        QTRY_VERIFY(!controller.channelCreationBusy()); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(done.size(), 0);
        const auto id = test.created.value("uuid").toString(); QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), id);
        QVERIFY(!controller.canCreateChannel());
        if (outcome == "terminal" || outcome == "promoted-existing") {
            QCOMPARE(controller.channelCreationPhase(), "members-failed"); QVERIFY(controller.canRetryChannelMembers());
            controller.checkChannelCreation(); QCOMPARE(test.bindingGets, 0);
        } else { QVERIFY(controller.channelCreationUncertain()); QVERIFY(!controller.canRetryChannelMembers()); }
        if (outcome == "promoted-existing") {
            auto a = creationBinding(channelMemberA, 1), b = creationBinding(channelMemberB, 2);
            a["stream_uuid"] = id; b["stream_uuid"] = id; a["role"] = "administrator"; b["role"] = "moderator";
            a["who_uuid"] = returnedId; b["who_uuid"] = returnedId; test.bindings = {a, b};
        }
        test.addStatus = 200; test.applyMembers = true; test.alterBindings = {};
        if (outcome == "terminal" || outcome == "promoted-existing") controller.retryChannelMembers();
        else controller.checkChannelCreation();
        if (outcome == "partial-503") {
            QTRY_COMPARE(controller.channelCreationPhase(), "members-required"); QCOMPARE(test.memberPosts, 1); QCOMPARE(done.size(), 0);
            controller.retryChannelMembers();
        }
        QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(controller.selectedStream(), id);
        QCOMPARE(test.memberPosts, outcome == "terminal" || outcome == "partial-503" ? 2 : 1);
        for (const auto &body : test.memberBodies) QCOMPARE(body, QJsonObject({{"member", QJsonArray{channelMemberA, channelMemberB}}}));
    }

    void channelMembershipDeletionFencesAnOlderSuccessfulAddResponse()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.addDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Member was removed", "", false, false, {channelMemberA, channelMemberB});
        QTRY_COMPARE(test.memberPosts, 1); QVERIFY(!test.bindings.isEmpty());
        const auto removed = test.bindings.first().toObject(); test.bindings.removeFirst();
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream_binding.deleted", QJsonObject{
            {"uuid", removed.value("uuid")}, {"stream_uuid", test.created.value("uuid")}, {"user_uuid", channelMemberA}})));
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0);
        controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        test.addDelay = 0; controller.retryChannelMembers(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 2);
    }

    void channelLiveMetadataDuringPublishCannotSaveAnOlderFreshCatalog()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        ChannelCreationFixture test; test.fixture.controller.useCache(&cache); QVERIFY(test.fixture.start()); QVERIFY(flushCache(cache));
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        bool injected = false;
        connect(&controller, &NativeController::streamsChanged, &controller, [&] {
            if (injected || test.created.isEmpty() || controller.channelCreationPhase() != "refreshing-catalogs") return;
            injected = true; test.created["name"] = "Live channel name";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", test.created)));
        });
        controller.createChannel("Older response name", "", false, false, {});
        QTRY_VERIFY(injected); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QVERIFY(flushCache(cache));
        const auto entry = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}});
        QVERIFY(!entry || !entry->fresh);
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
        const auto fresh = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}); QVERIFY(fresh); QVERIFY(fresh->fresh);
        bool found = false;
        for (const auto &value : fresh->rows) if (value.toObject().value("uuid") == test.created.value("uuid")) {
            found = true; QCOMPARE(value.toObject().value("name").toString(), "Live channel name");
        }
        QVERIFY(found);
    }

    void channelRouteChangeInsideActivationCannotClaimNavigationReady()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated); bool changed = false;
        connect(&controller, &NativeController::topicsChanged, &controller, [&] {
            if (changed || test.created.isEmpty() || controller.selectedStream() != test.created.value("uuid").toString()) return;
            changed = true; controller.selectStream(streamId);
        });
        controller.createChannel("Navigation interrupted", "", false, false, {});
        QTRY_VERIFY(changed); QTRY_VERIFY(!controller.channelCreationBusy()); QCOMPARE(done.size(), 0);
        QVERIFY(controller.channelCreationUncertain()); QVERIFY(controller.canCheckChannelCreation());
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
    }

    void channelBindingBatchUsesTheSourcePayloadUuidAsStreamIdentity()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.addDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Source batch identity", "", false, false, {channelMemberA, channelMemberB});
        QTRY_COMPARE(test.memberPosts, 1);
        // events.py create_stream_bindings_created_event emits uuid=stream UUID,
        // items=[full bindings], with no outer stream_uuid property.
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream_bindings.created", QJsonObject{
            {"uuid", test.created.value("uuid")}, {"items", test.bindings}})));
        QTRY_COMPARE(done.size(), 1); QCOMPARE(controller.channelCreationPhase(), "ready");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(test.bindingGets, 1);
    }

    void channelOwnAccessLossBeforeMemberDispatchStopsTheMutation_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"binding-role", "binding-delete", "stream-encryption", "stream-role", "stream-private"})
            QTest::newRow(change) << QString(change);
    }
    void channelOwnAccessLossBeforeMemberDispatchStopsTheMutation()
    {
        QFETCH(QString, change); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        bool injected = false;
        const auto before = controller.channelCreationContextKey();
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (injected || controller.channelCreationPhase() != "adding-members") return;
            injected = true;
            if (change.startsWith("binding")) {
                auto binding = creationBinding(userId, 900); binding["stream_uuid"] = test.created.value("uuid"); binding["role"] = "member";
                QVERIFY(controller.applyRealtimeEvent(controllerEvent(change == "binding-role" ? "stream_binding.updated" : "stream_binding.deleted", binding)));
            } else {
                auto stream = test.created;
                if (change == "stream-encryption") { stream["encryption"] = true; stream["current_encryption_key_uuid"] = returnedId; }
                else if (change == "stream-role") stream["role"] = "member";
                else { stream["private"] = true; stream["direct_user_uuid"] = returnedId; }
                QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream)));
            }
        });
        controller.createChannel("Do not add after access loss", "", false, false, {channelMemberA});
        QTRY_VERIFY(injected); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.memberPosts, 0);
        QVERIFY(!controller.canRetryChannelMembers()); QVERIFY(controller.channelCreationContextKey() != before);
        QCOMPARE(controller.channelCreationForm().value("memberUuids").toList(), QVariantList{channelMemberA});
    }

    void channelConfirmedStreamDeletionIsTerminalAndAllowsAnExplicitNewUuid()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.catalogDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto catalogs = test.streamGets;
        controller.createChannel("Deleted channel", "", false, false, {channelMemberA});
        QTRY_COMPARE(test.streamGets, catalogs + 1); const auto deleted = test.created.value("uuid").toString();
        QVERIFY(controller.channelCreationForm().value("confirmedStreamId").toString() == deleted);
        test.includeCreated = false;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.deleted", QJsonObject{{"uuid", deleted}})));
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(done.size(), 0);
        QVERIFY(controller.canCreateChannel()); QVERIFY(!controller.canCheckChannelCreation()); QVERIFY(!controller.canRetryChannelMembers());
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        test.catalogDelay = 0; test.includeCreated = true;
        controller.createChannel("Explicit new attempt", "", true, false, {channelMemberA});
        QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 2);
        QVERIFY(test.created.value("uuid").toString() != deleted);
        QCOMPARE(test.streamBodies.first().value("uuid").toString(), deleted);
        QCOMPARE(test.streamBodies.last().value("uuid").toString(), controller.selectedStream());
    }

    void channelDefaultDeletionRetainsTheStreamAndAllowsGetOnlyRecovery()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.catalogDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto catalogs = test.streamGets;
        controller.createChannel("Default was removed", "", false, false, {channelMemberA});
        QTRY_COMPARE(test.streamGets, catalogs + 1); const auto id = test.created.value("uuid").toString();
        test.created["default_topic_uuid"] = QJsonValue::Null; test.includeDefault = false;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", channelDefault}, {"stream_uuid", id}})));
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(done.size(), 0);
        QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), id);
        QVERIFY(!controller.canCreateChannel()); QVERIFY(controller.canCheckChannelCreation()); QVERIFY(!controller.canRetryChannelMembers());
        test.catalogDelay = 0; controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        // Another authorized caller selects an existing replacement default.
        test.created["default_topic_uuid"] = returnedId; test.defaultTopic["uuid"] = returnedId; test.includeDefault = true;
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(controller.selectedStream(), id);
        QCOMPARE(controller.channelCreationForm().value("defaultTopicId").toString(), returnedId);
    }

    void channelMatchingOwnCreationRealtimeSettlesWithoutManualCheck_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"canonical", "default", "catalog"}) QTest::newRow(stage) << QString(stage);
    }
    void channelMatchingOwnCreationRealtimeSettlesWithoutManualCheck()
    {
        QFETCH(QString, stage); ChannelCreationFixture test;
        RealtimeGateway gateway(test.fixture.server.serverPort()); QVector<QJsonObject> events;
        test.fixture.realtime = [&events](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows; const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : events) if (event.value("epoch_version").toInt() > after) rows.append(event);
            return Response{rows};
        };
        QVERIFY(test.fixture.start(gateway.origin())); auto &controller = test.fixture.controller;
        RealtimeClient realtime(&test.fixture.api, &test.fixture.session); controller.useRealtime(&realtime);
        QTRY_COMPARE(controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150; else test.catalogDelay = 150;
        const auto streams = test.streamGets, users = test.usersGets, history = test.fixture.totalHistoryGets();
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Ordinary own realtime", "", true, false, {channelMemberA, channelMemberB});
        if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1); else QTRY_COMPARE(test.streamGets, streams + 1);
        const auto event = stage == "canonical" ? controllerEvent("stream.created", test.created, 11)
            : stage == "default" ? controllerEvent("topic.created", test.defaultTopic, 11)
            : controllerEvent("stream_bindings.created", QJsonObject{{"uuid", test.created.value("uuid")}, {"items", test.bindings}}, 11);
        events.append(event); gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(11)); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY2(controller.channelCreationPhase() == "ready", qPrintable(controller.channelCreationPhase()
            + QString(" [stream GET %1 / topic GET %2 / catalog GET %3]").arg(test.gets).arg(test.topicGets).arg(test.streamGets - streams)));
        QCOMPARE(done.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(test.usersGets, users);
        QVERIFY(test.gets <= 2); QVERIFY(test.topicGets <= 2); QVERIFY(test.streamGets - streams <= 2);
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1);
    }

    void channelCompletedAttemptDoesNotCaptureAnUnrelatedNewFormLifetime()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Already complete", "", false, false, {}); QTRY_COMPARE(done.size(), 1);
        controller.selectStream(streamId); QTRY_VERIFY(!controller.busy()); const auto key = controller.channelCreationContextKey();
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{
            {"uuid", channelDefault}, {"stream_uuid", test.created.value("uuid")}})));
        QCOMPARE(controller.channelCreationContextKey(), key); QVERIFY(controller.canCreateChannel());
        QCOMPARE(controller.channelCreationPhase(), "ready");
    }

    void channelOwnReadReconciliationIsBoundedAndNeverReplaysWrites()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.getDelay = 40;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&test, &controller, fallback](const Request &request) {
            auto response = fallback(request);
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/streams/" + test.created.value("uuid").toString()) {
                const auto row = test.created;
                QTimer::singleShot(0, &controller, [&controller, row] {
                    controller.applyRealtimeEvent(controllerEvent("stream.created", row));
                });
            }
            return response;
        };
        controller.createChannel("Bounded reconciliation", "", false, false, {channelMemberA});
        QTRY_VERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.gets, 4); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); QCOMPARE(done.size(), 0);
        QVERIFY(controller.channelCreationUncertain()); QVERIFY(controller.canCheckChannelCreation());
        test.fixture.server.handler = fallback; test.getDelay = 0;
        controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        controller.retryChannelMembers(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
    }

    void channelUnsafeAbaCannotBeErasedByMatchingOwnEvents_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"canonical", "default", "members", "catalog"}) QTest::newRow(stage) << QString(stage);
    }
    void channelUnsafeAbaCannotBeErasedByMatchingOwnEvents()
    {
        QFETCH(QString, stage); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150;
        else if (stage == "members") test.addDelay = 150; else test.catalogDelay = 150;
        const auto streams = test.streamGets;
        controller.createChannel("Do not accept older A", "", false, false, {channelMemberA});
        if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1);
        else if (stage == "members") QTRY_COMPARE(test.memberPosts, 1); else QTRY_COMPARE(test.streamGets, streams + 1);
        auto changed = test.created; changed["description"] = "Newer B";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", changed)));
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", test.created)));
        // A subsequent matching own event must not turn an unsafe ABA into a
        // harmless retry. Neither value equality nor last-event-kind is a fence.
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.created", test.created)));
        QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QCOMPARE(test.gets, 1); QVERIFY(test.memberPosts <= 1);
        test.getDelay = test.topicDelay = test.addDelay = test.catalogDelay = 0;
        controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.posts, 1);
        if (test.memberPosts) { QCOMPARE(done.size(), 1); QCOMPARE(test.memberPosts, 1); }
        else { QCOMPARE(controller.channelCreationPhase(), "members-required"); QVERIFY(controller.canRetryChannelMembers()); }
    }

    void channelCollectionTraversalHasResourceCeilings_data()
    {
        QTest::addColumn<QString>("resource"); QTest::addColumn<QString>("stage");
        for (const auto *stage : {"default", "bindings", "catalog"})
            for (const auto *resource : {"repeated-marker", "pages", "rows", "bytes"})
                QTest::newRow(qPrintable(QString(stage) + '-' + resource)) << QString(resource) << QString(stage);
    }
    void channelCollectionTraversalHasResourceCeilings()
    {
        QFETCH(QString, resource); QFETCH(QString, stage); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto fallback = test.fixture.server.handler; int requests = 0;
        test.fixture.server.handler = [&test, fallback, &requests, resource, stage](const Request &request) {
            const auto path = request.target.path();
            const bool target = !test.created.isEmpty() && request.method == "GET"
                && ((stage == "default" && path.endsWith("/stream_topics/"))
                    || (stage == "bindings" && path.endsWith("/stream_bindings/"))
                    || (stage == "catalog" && path.endsWith("/streams/")));
            if (!target) return fallback(request);
            ++requests; QJsonArray rows;
            if (resource == "rows") for (int n = 0; n < 1000; ++n) rows.append(QJsonObject{});
            if (resource == "bytes") rows.append(QJsonObject{{"padding", QString(1024 * 1024, 'x')}});
            return Response{rows, 200, 0, {{"X-Pagination-Marker", resource == "repeated-marker" ? QByteArray("same") : QByteArray::number(requests)}}};
        };
        if (stage == "bindings") test.postStatus = 503;
        QSignalSpy changed(&controller, &NativeController::stateChanged);
        controller.createChannel("Bounded catalogs", "", false, false, {channelMemberA});
        if (stage == "bindings") {
            QTRY_VERIFY(controller.channelCreationUncertain()); controller.checkChannelCreation();
        }
        // Wait for actual completions rather than adding polling latency to
        // 500 local requests. The same default timeout bounds the whole walk.
        while (controller.channelCreationBusy()) QVERIFY(changed.wait());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QVERIFY(requests > 0); QVERIFY(requests <= 500);
        if (resource == "pages") QCOMPARE(requests, 500);
        if (resource == "repeated-marker") QCOMPARE(requests, 2);
        if (resource == "rows") QCOMPARE(requests, 101);
        if (resource == "bytes") QCOMPARE(requests, 32);
        QCOMPARE(test.memberPosts, stage == "catalog" ? 1 : 0);
    }

    void channelContextLossFencesEveryDelayedStage_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("change");
        for (const auto *stage : {"post", "canonical", "default", "members", "bindings", "catalog"})
            for (const auto *change : {"route", "token", "logout", "cache-replace", "cache-destroy", "snapshot"})
                QTest::newRow(qPrintable(QString(stage) + '-' + change)) << QString(stage) << QString(change);
    }
    void channelContextLossFencesEveryDelayedStage()
    {
        QFETCH(QString, stage); QFETCH(QString, change);
        QTemporaryDir first, second;
        auto cache = std::make_unique<NativeCacheService>(first.path(), NativeCache::Policy{}, true);
        NativeCacheService replacement(second.path(), {}, true);
        ChannelCreationFixture test;
        if (change.startsWith("cache-")) test.fixture.controller.useCache(cache.get());
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        if (stage == "post") test.postDelay = 150;
        else if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150;
        else if (stage == "members") test.addDelay = 150;
        else if (stage == "bindings") { test.postStatus = 503; test.bindingDelay = 150; }
        else test.catalogDelay = 150;
        const auto streams = test.streamGets;
        controller.createChannel("Frozen stage context", "", false, true, {channelMemberA});
        if (stage == "bindings") { QTRY_VERIFY(controller.channelCreationUncertain()); controller.checkChannelCreation(); }
        if (stage == "post") QTRY_COMPARE(test.posts, 1);
        else if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1);
        else if (stage == "members") QTRY_COMPARE(test.memberPosts, 1);
        else if (stage == "bindings") QTRY_COMPARE(test.bindingGets, 1);
        else QTRY_COMPARE(test.streamGets, streams + 1);
        const auto form = controller.channelCreationForm(); const auto context = controller.channelCreationContextKey();
        if (change == "route") controller.selectStream({});
        else if (change == "logout") controller.logout();
        else if (change == "cache-replace") controller.useCache(&replacement);
        else if (change == "cache-destroy") cache.reset();
        else if (change == "snapshot") controller.reloadSnapshot();
        else {
            test.fixture.authentication = [](const Request &request) {
                if (request.target.path().endsWith("/actions/get_token/invoke"))
                    return Response{QJsonObject{{"access_token", "rotated-channel-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
                return sessionResponse(request);
            };
            test.fixture.session.requestRefresh();
        }
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!controller.channelCreationBusy()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QCOMPARE(test.memberPosts, stage == "members" || stage == "catalog" ? 1 : 0);
        QVERIFY(controller.selectedStream() != test.created.value("uuid").toString());
        QVERIFY(controller.channelCreationContextKey() != context);
        if (change == "logout") { QVERIFY(controller.channelCreationForm().isEmpty()); return; }
        QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(controller.channelCreationForm().value("attemptId"), form.value("attemptId"));
        QCOMPARE(controller.channelCreationForm().value("memberUuids"), form.value("memberUuids"));
        controller.createChannel("Do not replace an unknown attempt", "", false, false, {});
        QCOMPARE(test.posts, 1); QCOMPARE(controller.channelCreationForm().value("attemptId"), form.value("attemptId"));
    }

    void channelCreatedCatalogsSurviveColdMessengerFailureWithoutTrustingCachedAcl()
    {
        QTemporaryDir directory; ChannelCreationFixture test; QVERIFY(test.fixture.start()); QString id;
        {
            NativeCacheService cache(directory.path(), {}, true);
            NativeController current(&test.fixture.api, &test.fixture.session); current.useCache(&cache);
            QTRY_COMPARE(current.streams().size(), 1); QTRY_VERIFY(current.canCreateChannel()); QTRY_VERIFY(current.channelMembersVerified());
            QSignalSpy done(&current, &NativeController::channelCreated);
            current.createChannel("Saved new channel", "", true, false, {channelMemberA}); QTRY_COMPARE(done.size(), 1);
            id = current.selectedStream(); QVERIFY(flushCache(cache));
            QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}));
            QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, id, {}, {}}));
        }
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback](const Request &request) {
            if (request.method == "GET" && (request.target.path().contains("/messenger/") || request.target.path().endsWith("/users/")))
                return Response{QJsonObject{}, 503};
            return fallback(request);
        };
        NativeCacheService reopened(directory.path(), {}, true);
        NativeController restored(&test.fixture.api, &test.fixture.session); restored.useCache(&reopened);
        QTRY_COMPARE(restored.streams().size(), 2); QVERIFY(!restored.canCreateChannel()); QVERIFY(restored.cacheStale());
        restored.selectStream(id); QTRY_COMPARE(restored.topics().size(), 1);
        QCOMPARE(restored.topics().first().toMap().value("id").toString(), channelDefault);
        QVERIFY(!restored.canCreateTopicIn(id)); QVERIFY(!restored.canSend()); QVERIFY(!restored.canAttach());
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
    }

    void channelOwnerOrProjectChangeCannotPublishAnOlderStage_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<bool>("projectChange");
        for (const auto *stage : {"post", "canonical", "default", "members", "bindings", "catalog"}) {
            QTest::newRow(qPrintable(QString(stage) + "-owner")) << QString(stage) << false;
            QTest::newRow(qPrintable(QString(stage) + "-project")) << QString(stage) << true;
        }
    }
    void channelOwnerOrProjectChangeCannotPublishAnOlderStage()
    {
        QFETCH(QString, stage); QFETCH(bool, projectChange); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        if (stage == "post") test.postDelay = 150;
        else if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150;
        else if (stage == "members") test.addDelay = 150;
        else if (stage == "bindings") { test.postStatus = 503; test.bindingDelay = 150; }
        else test.catalogDelay = 150;
        const auto catalogs = test.streamGets;
        controller.createChannel("Belongs to the original identity", "", false, false, {channelMemberA});
        if (stage == "bindings") { QTRY_VERIFY(controller.channelCreationUncertain()); controller.checkChannelCreation(); }
        if (stage == "post") QTRY_COMPARE(test.posts, 1);
        else if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1);
        else if (stage == "members") QTRY_COMPARE(test.memberPosts, 1);
        else if (stage == "bindings") QTRY_COMPARE(test.bindingGets, 1);
        else QTRY_COMPARE(test.streamGets, catalogs + 1);
        const auto form = controller.channelCreationForm();
        const auto newProject = projectChange ? returnedId : projectId;
        const auto newUser = projectChange ? userId : returnedId;
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback, newProject, newUser](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            if (path.endsWith("/actions/introspect")) {
                auto raw = result.data.toObject(); raw["project_id"] = newProject;
                raw["user_info"] = QJsonObject{{"uuid", newUser}}; result.data = raw;
            } else if (path == "/api/core/v1/iam/projects/") {
                result.data = QJsonArray{QJsonObject{{"uuid", newProject}, {"name", "Another project"}, {"status", "ACTIVE"}}};
            } else if (path.endsWith("/streams/") && request.method == "GET") {
                QJsonArray rows;
                for (const auto &value : result.data.toArray()) {
                    auto raw = value.toObject(); raw["project_id"] = newProject; raw["user_uuid"] = newUser; raw["owner"] = newUser; rows.append(raw);
                }
                result.data = rows; result.delay = 0;
            }
            return result;
        };
        controller.login(test.fixture.server.origin().toString(), "another-synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(newProject);
        QTRY_VERIFY(controller.authenticated()); QTRY_COMPARE(controller.streams().size(), 2);
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(controller.channelCreationForm().isEmpty()); QCOMPARE(controller.channelCreationPhase(), "idle");
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        std::function<void()> releaseDirectory;
        test.fixture.server.handler = [fallback, &releaseDirectory](const Request &request) {
            auto response = fallback(request);
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/users/") {
                response.defer = [&releaseDirectory](auto send) { releaseDirectory = std::move(send); };
            }
            return response;
        };
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_VERIFY(controller.authenticated()); QTRY_COMPARE(controller.streams().size(), 2);
        QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(controller.channelCreationForm().value("attemptId"), form.value("attemptId"));
        QCOMPARE(controller.channelCreationForm().value("memberUuids"), form.value("memberUuids"));
        test.postDelay = test.getDelay = test.topicDelay = test.addDelay = test.bindingDelay = test.catalogDelay = 0;
        QTRY_VERIFY(static_cast<bool>(releaseDirectory));
        QVERIFY(!controller.canCheckChannelCreation());
        releaseDirectory();
        QTRY_VERIFY(controller.canCheckChannelCreation());
        controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.posts, 1);
        if (stage == "members" || stage == "catalog") QCOMPARE(done.size(), 1);
        else { QCOMPARE(controller.channelCreationPhase(), "members-required"); QCOMPARE(done.size(), 0); }
    }

    void channelFollowupDtosRejectMalformedIdentityAndMembership_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        for (const auto *stage : {"default", "members", "bindings", "catalog"}) {
            const auto add = [stage](const char *field, QJsonValue value) {
                QTest::newRow(qPrintable(QString(stage) + '-' + field)) << QString(stage) << QString(field) << value;
            };
            add("project_id", returnedId); add("uuid", "not-a-uuid"); add("created_at", "bad-date");
            add("updated_at", 42); add("duplicate", QJsonValue::Null);
            if (QString(stage) != "catalog") add("stream_uuid", returnedId);
            if (QString(stage) == "default") {
                add("user_uuid", returnedId); add("is_default", false); add("is_done", "false");
                add("notification_mode", "unknown-mode"); add("unread_count", -1); add("name", QString(129, 'x'));
            } else if (QString(stage) == "members" || QString(stage) == "bindings") {
                add("user_uuid", "not-a-uuid"); add("who_uuid", "not-a-uuid"); add("role", "unknown-role");
                add("notification_mode", "unknown-mode");
            } else { add("owner", returnedId); add("default_topic_uuid", returnedId); add("role", "guest"); }
        }
    }
    void channelFollowupDtosRejectMalformedIdentityAndMembership()
    {
        QFETCH(QString, stage); QFETCH(QString, field); QFETCH(QJsonValue, value);
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated); const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&test, stage, field, value, fallback](const Request &request) {
            auto response = fallback(request); const auto path = request.target.path();
            const bool target = !test.created.isEmpty()
                && ((stage == "default" && path.endsWith("/stream_topics/"))
                    || (stage == "members" && path.endsWith("/actions/add_users/invoke"))
                    || (stage == "bindings" && path.endsWith("/stream_bindings/"))
                    || (stage == "catalog" && path.endsWith("/streams/") && request.method == "GET"));
            if (target) {
                auto rows = response.data.toArray(); auto row = rows.last().toObject();
                if (field == "duplicate") rows.append(row);
                else { row[field] = value; rows.replace(rows.size() - 1, row); }
                response.data = rows;
            }
            return response;
        };
        if (stage == "bindings") test.postStatus = 503;
        controller.createChannel("Strict follow-up DTOs", "", false, false, {channelMemberA});
        if (stage == "bindings") {
            QTRY_VERIFY(controller.channelCreationUncertain());
            auto row = creationBinding(channelMemberA, 100); row["stream_uuid"] = test.created.value("uuid"); test.bindings = QJsonArray{row};
            controller.checkChannelCreation();
        }
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QVERIFY(controller.selectedStream() != test.created.value("uuid").toString());
        QCOMPARE(test.memberPosts, stage == "members" || stage == "catalog" ? 1 : 0);
    }

    void channelCreationSubscribersMayDestroyTheController_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const auto *boundary : {"creating", "verifying-stream", "verifying-default", "adding-members",
                 "checking-members", "refreshing-catalogs", "streams", "topics", "ready", "uncertain", "completed"})
            QTest::newRow(boundary) << QString(boundary);
    }
    void channelCreationSubscribersMayDestroyTheController()
    {
        QFETCH(QString, boundary); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> current = new NativeController(&test.fixture.api, &test.fixture.session);
        const auto cleanup = qScopeGuard([&current] { delete current; });
        current->refresh();
        QTRY_COMPARE(current->streams().size(), 1); QTRY_VERIFY(current->channelMembersVerified());
        if (boundary == "checking-members" || boundary == "uncertain") test.postStatus = 503;
        QObject::connect(current, &NativeController::stateChanged, current, [&current, boundary] {
            if (current && current->channelCreationPhase() == boundary) delete current;
        });
        QObject::connect(current, &NativeController::streamsChanged, current, [&current, &test, boundary] {
            if (boundary != "streams" || !current || test.created.isEmpty()) return;
            for (const auto &row : current->streams()) if (row.toMap().value("id") == test.created.value("uuid").toVariant()) { delete current; return; }
        });
        QObject::connect(current, &NativeController::topicsChanged, current, [&current, &test, boundary] {
            if (boundary == "topics" && current && current->selectedStream() == test.created.value("uuid").toString()) delete current;
        });
        QObject::connect(current, &NativeController::channelCreated, current, [&current, boundary] {
            if (boundary == "completed") delete current;
        });
        current->createChannel("Subscriber lifetime", "", false, false, {channelMemberA});
        if (boundary == "checking-members") {
            QTRY_VERIFY(current && current->channelCreationUncertain()); current->checkChannelCreation();
        }
        QTRY_VERIFY(current.isNull()); QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.posts, boundary == "creating" ? 0 : 1); QVERIFY(test.memberPosts <= 1);
    }

    void channelAllOwnCreationEventsShareOneBoundedReadOnlyReconciliationBudget()
    {
        ChannelCreationFixture test; RealtimeGateway gateway(test.fixture.server.serverPort()); QVector<QJsonObject> events;
        test.fixture.realtime = [&events](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows; const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : events) if (event.value("epoch_version").toInt() > after) rows.append(event);
            return Response{rows};
        };
        QVERIFY(test.fixture.start(gateway.origin())); auto &controller = test.fixture.controller;
        RealtimeClient realtime(&test.fixture.api, &test.fixture.session); controller.useRealtime(&realtime);
        QTRY_COMPARE(controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        test.getDelay = test.topicDelay = test.addDelay = 70;
        const auto fallback = test.fixture.server.handler; QSet<QString> sent;
        test.fixture.server.handler = [&test, &controller, &events, &gateway, &sent, fallback](const Request &request) {
            auto result = fallback(request); if (test.created.isEmpty()) return result;
            const auto path = request.target.path(); QString kind; QJsonObject payload;
            if (request.method == "GET" && path == "/api/workspace/v1/messenger/streams/" + test.created.value("uuid").toString()) {
                kind = "stream.created"; payload = test.created;
            } else if (request.method == "GET" && path.endsWith("/stream_topics/")) {
                kind = "topic.created"; payload = test.defaultTopic;
            } else if (request.method == "POST" && path.endsWith("/actions/add_users/invoke")) {
                kind = "stream_bindings.created"; payload = {{"uuid", test.created.value("uuid")}, {"items", test.bindings}};
            }
            if (!kind.isEmpty() && !sent.contains(kind)) {
                sent.insert(kind); const auto event = controllerEvent(kind, payload, 10 + sent.size()); events.append(event);
                QTimer::singleShot(0, &controller, [&gateway, event] {
                    gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
                });
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::channelCreated); const auto streams = test.streamGets;
        controller.createChannel("All own events", "", false, false, {channelMemberA, channelMemberB});
        QTRY_COMPARE(done.size(), 1); QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(13));
        QCOMPARE(controller.channelCreationPhase(), "ready"); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        QCOMPARE(test.gets, 2); QCOMPARE(test.topicGets, 2); QCOMPARE(test.bindingGets, 1); QCOMPARE(test.streamGets, streams + 1);
    }

    void channelPostErrorsDistinguishDefiniteFailureFromUnknownOutcome_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("definite");
        for (const int status : {400, 401, 403, 404, 409, 429, 500, 503})
            QTest::newRow(qPrintable(QString::number(status))) << status << (status == 400 || status == 401 || status == 403 || status == 404);
    }
    void channelPostErrorsDistinguishDefiniteFailureFromUnknownOutcome()
    {
        QFETCH(int, status); QFETCH(bool, definite); ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = status;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("One definite attempt", "", false, false, {channelMemberA});
        QTRY_VERIFY(!controller.channelCreationBusy()); const auto id = test.created.value("uuid").toString();
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); QCOMPARE(test.gets, 0); QCOMPARE(done.size(), 0);
        QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), QString{});
        QVERIFY(controller.channelCreateRequestFailed());
        QCOMPARE(controller.canCreateChannel(), definite); QCOMPARE(controller.canCheckChannelCreation(), !definite);
        if (definite) {
            QCOMPARE(controller.channelCreationPhase(), "failed"); test.postStatus = 201;
            controller.createChannel("Explicit new attempt", "", false, false, {}); QTRY_COMPARE(done.size(), 1);
            QCOMPARE(test.posts, 2); QVERIFY(test.created.value("uuid").toString() != id);
        } else {
            controller.createChannel("Do not replay", "", false, false, {}); QCOMPARE(test.posts, 1);
            controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
            QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        }
    }

    void channelPagedCatalogsPublishOnceAndWarmRevisitAvoidsRedundantGet_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"default", "bindings", "catalog"}) QTest::newRow(stage) << QString(stage);
    }
    void channelPagedCatalogsPublishOnceAndWarmRevisitAvoidsRedundantGet()
    {
        QFETCH(QString, stage); QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        ChannelCreationFixture test; test.fixture.controller.useCache(&cache); QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; const auto fallback = test.fixture.server.handler; int pages = 0; bool validMarker = true;
        test.fixture.server.handler = [&test, fallback, stage, &pages, &validMarker](const Request &request) {
            auto response = fallback(request); const auto path = request.target.path();
            const bool target = !test.created.isEmpty() && request.method == "GET"
                && ((stage == "default" && path.endsWith("/stream_topics/"))
                    || (stage == "bindings" && path.endsWith("/stream_bindings/"))
                    || (stage == "catalog" && path.endsWith("/streams/")));
            if (target) {
                ++pages; auto rows = response.data.toArray();
                if (QUrlQuery(request.target).queryItemValue("page_marker").isEmpty()) {
                    // An empty page with a next cursor is valid and bounded.
                    response.data = QJsonArray{}; response.headers["X-Pagination-Marker"] = "next-valid-page";
                } else {
                    validMarker &= QUrlQuery(request.target).queryItemValue("page_marker") == "next-valid-page";
                    response.data = rows;
                }
            }
            return response;
        };
        if (stage == "bindings") test.postStatus = 503;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Paged and cached channel", "", false, false, stage == "bindings" ? QVariantList{channelMemberA} : QVariantList{});
        if (stage == "bindings") {
            QTRY_VERIFY(controller.channelCreationUncertain()); auto row = creationBinding(channelMemberA, 100);
            row["stream_uuid"] = test.created.value("uuid"); test.bindings = {row}; controller.checkChannelCreation();
        }
        QTRY_COMPARE(done.size(), 1); QCOMPARE(pages, 2); QVERIFY(validMarker); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache)); const auto id = controller.selectedStream();
        controller.selectStream(streamId); QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache));
        const auto topics = test.topicGets, history = test.fixture.totalHistoryGets(), catalogs = test.streamGets, users = test.usersGets;
        controller.selectStream(id); QTRY_VERIFY(!controller.busy()); QTRY_COMPARE(controller.topics().size(), 1);
        QCOMPARE(test.topicGets, topics); QCOMPARE(test.fixture.totalHistoryGets(), history);
        QCOMPARE(test.streamGets, catalogs); QCOMPARE(test.usersGets, users); QCOMPARE(test.posts, 1); QCOMPARE(done.size(), 1);
        QCOMPARE(controller.conversationKind(), "stream"); QVERIFY(!controller.canSend());
    }

    void channelDefaultChangeBetweenReadStagesCannotPublishTheEarlierTopicSnapshot_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const auto *boundary : {"adding-members", "checking-members", "refreshing-catalogs"})
            QTest::newRow(boundary) << QString(boundary);
    }
    void channelDefaultChangeBetweenReadStagesCannotPublishTheEarlierTopicSnapshot()
    {
        QFETCH(QString, boundary); ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated); bool updated = false;
        if (boundary == "checking-members") test.postStatus = 503;
        QObject::connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (updated || controller.channelCreationPhase() != boundary) return;
            updated = true; test.defaultTopic["name"] = "Newer topic metadata";
            controller.applyRealtimeEvent(controllerEvent("topic.updated", test.defaultTopic));
        });
        controller.createChannel("A complete snapshot must stay current", "", false, false,
            boundary == "refreshing-catalogs" ? QVariantList{} : QVariantList{channelMemberA});
        if (boundary == "checking-members") {
            QTRY_VERIFY(controller.channelCreationUncertain()); auto row = creationBinding(channelMemberA, 100);
            row["stream_uuid"] = test.created.value("uuid"); test.bindings = {row}; controller.checkChannelCreation();
        }
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(updated);
        QCOMPARE(done.size(), 0); QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(test.posts, 1); QVERIFY(test.memberPosts <= 1);
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(controller.topics().first().toMap().value("name").toString(), QString("Newer topic metadata"));
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, boundary == "adding-members" ? 1 : 0);
    }

    // ---- Issue #635: topic-summary settings controller surface ----

    void topicSummaryGatesRoundTripAndErrorMapping_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<QString>("kind");
        QTest::newRow("ok") << 200 << QString("saved");
        QTest::newRow("bad-request") << 400 << QString("invalid");
        QTest::newRow("forbidden") << 403 << QString("forbidden");
        QTest::newRow("server") << 503 << QString("server");
    }
    void topicSummaryGatesRoundTripAndErrorMapping()
    {
        QFETCH(int, status); QFETCH(QString, kind);
        TopicSummaryScene scene; scene.permissions = {QJsonValue("workspace.topic_summary_settings.manage")};
        scene.gatesStatus = status; QVERIFY(scene.fixture.start());
        auto &controller = scene.fixture.controller;
        controller.loadTopicSummaryGates();
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(scene.gatesGets, 1);
        if (status != 200) {
            QCOMPARE(controller.topicSummarySettingsStatus(), QString("error"));
            QCOMPARE(controller.topicSummarySettingsError(), kind);
            QVERIFY(controller.topicSummaryGates().isEmpty());
            return;
        }
        QCOMPARE(controller.topicSummaryGates().value("globalEnabled"), QVariant(true));
        QCOMPARE(controller.topicSummaryGates().value("projectEnabled"), QVariant(false));
        QCOMPARE(controller.topicSummaryGates().value("projectId"), QVariant(projectId));
        // Save sends both gates and only merges the echo booleans back.
        scene.gatesResponse["project_enabled"] = true;
        controller.saveTopicSummaryGates(true, true);
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(scene.gatesPuts, 1);
        QCOMPARE(scene.lastGatesPut, QJsonObject({{"global_enabled", true}, {"project_enabled", true}}));
        QCOMPARE(controller.topicSummarySettingsStatus(), QString("saved"));
        QCOMPARE(controller.topicSummaryGates().value("projectEnabled"), QVariant(true));
    }

    void topicSummaryGatesSchemaMismatchReportsContract()
    {
        TopicSummaryScene scene; scene.permissions = {QJsonValue("workspace.topic_summary_settings.manage")};
        scene.gatesResponse = QJsonObject{{"project_id", projectId}, {"global_enabled", "true"}};
        QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
        controller.loadTopicSummaryGates();
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(controller.topicSummarySettingsStatus(), QString("error"));
        QCOMPARE(controller.topicSummarySettingsError(), QString("contract"));
    }

    // Issue #736: a token renewal for the SAME account retires the transport
    // generation, so the wrapper never answers the in-flight gates callback and
    // nothing released the shared busy flag: the retry was ignored and both
    // tabs stayed locked. The renewal must settle the retired summary
    // operations with a retryable outcome, fence the late reply, and let the
    // renewed session load the gates again without a restart.
    void topicSummaryGatesRenewalReleasesBusyAndRetries()
    {
        TopicSummaryScene scene; scene.permissions = {QJsonValue("workspace.topic_summary_settings.manage")};
        bool deferFirst = true;
        std::function<void()> release;
        const auto base = scene.fixture.authentication;
        scene.fixture.authentication = [&](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path().contains("topic_summary_settings")
                && deferFirst) {
                deferFirst = false;
                ++scene.gatesGets;
                Response response{scene.gatesResponse};
                response.defer = [&](auto finish) { release = std::move(finish); };
                return response;
            }
            return base(request);
        };
        QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
        controller.loadTopicSummaryGates();
        QTRY_COMPARE(scene.gatesGets, 1);
        QVERIFY(bool(release));
        QVERIFY(controller.topicSummarySettingsBusy());
        // Same-owner renewal: a replacement access token bumps the generation.
        scene.fixture.api.setAccessToken("summary-736-renewed-access-token");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicSummarySettingsBusy(), 4000);
        QCOMPARE(controller.topicSummarySettingsStatus(), QString("error"));
        QCOMPARE(controller.topicSummarySettingsError(), QString("interrupted"));
        // Unverified gates stay uneditable: the loaded flag cannot be set by a
        // reply that never settled (review #708 finding 8 contract).
        QVERIFY(!controller.topicSummaryGatesLoaded());
        const auto statusAfterSettle = controller.topicSummarySettingsStatus();
        // The late reply of the retired generation must settle nothing and must
        // not apply its values to the renewed session.
        release();
        QTest::qWait(300);
        QCOMPARE(controller.topicSummarySettingsStatus(), statusAfterSettle);
        QCOMPARE(controller.topicSummarySettingsError(), QString("interrupted"));
        QVERIFY(!controller.topicSummaryGatesLoaded());
        // The renewed session loads the gates again with no restart.
        controller.loadTopicSummaryGates();
        QTRY_COMPARE(scene.gatesGets, 2);
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(controller.topicSummarySettingsStatus(), QString(""));
        QVERIFY(controller.topicSummaryGatesLoaded());
    }

    void topicSummaryInterruptedCreateReconcilesBeforeRetry()
    {
        // exordy medium on #755 (finding 4670644138): a create whose request
        // died mid-transport may still have committed server-side. An
        // immediate retry must not blindly issue a second POST: the list is
        // reconciled first, and only then is a fresh create allowed again.
        TopicSummaryScene scene;
        scene.permissions = {QJsonValue("workspace.topic_summary_endpoint.manage")};
        QJsonArray served; // committed server-side rows
        bool deferCreate = true;
        std::function<void()> releaseCreate;
        const auto base = scene.fixture.authentication;
        scene.fixture.authentication = [&](const Request &request) -> Response {
            if (request.target.path().contains("topic_summary_endpoints")) {
                if (request.method == "POST") {
                    scene.endpointPosts.append(request.body);
                    auto committed = summaryEndpointRow(request.body.value("uuid").toString());
                    committed.insert("name", request.body.value("name"));
                    served.append(committed); // the server commits on receipt
                    Response response{QJsonObject{}, 200};
                    if (deferCreate) {
                        deferCreate = false;
                        response.defer = [&](auto finish) { releaseCreate = std::move(finish); };
                    }
                    return response;
                }
                ++scene.endpointGets;
                return Response{served, 200};
            }
            return base(request);
        };
        QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
        controller.loadTopicSummaryEndpoints();
        QTRY_COMPARE(scene.endpointGets, 1);
        const QVariantMap form{{"name", "dup-guard"}, {"base_url", "https://summary.example/api/v1"},
            {"model", "m"}, {"api_key", "k"}};
        QVERIFY(controller.createTopicSummaryEndpoint(form));
        QTRY_COMPARE(scene.endpointPosts.size(), qsizetype(1));
        QVERIFY(controller.topicSummarySettingsBusy());
        // Same-owner renewal retires the transport: the create settles as
        // interrupted although the server already committed the row.
        scene.fixture.api.setAccessToken("summary-755-renewed-access-token");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicSummarySettingsBusy(), 4000);
        QCOMPARE(controller.topicSummarySettingsError(), QString("interrupted"));
        // The retry must NOT post blindly: it triggers the list reconcile.
        QVERIFY(!controller.createTopicSummaryEndpoint(form));
        QCOMPARE(scene.endpointPosts.size(), 1);
        QTRY_COMPARE(scene.endpointGets, 2);
        // Reconcile proves the create committed: the row is visible instead
        // of being silently duplicated.
        QTRY_COMPARE(controller.topicSummaryEndpoints().size(), qsizetype(1));
        QCOMPARE(controller.topicSummaryEndpoints().first().toMap().value("name").toString(),
                 QString("dup-guard"));
        // The late reply of the retired create must not append a second row.
        releaseCreate();
        QTest::qWait(300);
        QCOMPARE(controller.topicSummaryEndpoints().size(), qsizetype(1));
        // Reconciled: a fresh create is dispatched again.
        QVERIFY(controller.createTopicSummaryEndpoint(
            {{"name", "second"}, {"base_url", "https://summary.example/api/v1"}, {"model", "m"},
             {"api_key", "k"}}));
        QTRY_COMPARE(scene.endpointPosts.size(), qsizetype(2));
    }

    void topicSummaryGatePermissionsFollowCapabilitiesAndRole()
    {
        TopicSummaryScene scene; QVERIFY(scene.fixture.start());
        auto &controller = scene.fixture.controller;
        // Owner of own stream: the topic tab is allowed even without capabilities.
        QCOMPARE(controller.topicSummaryTabPermission("topic"), QString("allowed"));
        QCOMPARE(controller.topicSummaryTabPermission("gates"), QString("denied"));
        QCOMPARE(controller.topicSummaryTabPermission("endpoints"), QString("denied"));
        // Gated calls short-circuit without traffic.
        controller.loadTopicSummaryGates();
        QCOMPARE(scene.gatesGets, 0);
        QCOMPARE(controller.topicSummarySettingsError(), QString("forbidden"));
        controller.saveTopicSummaryGates(true, false);
        QCOMPARE(scene.gatesPuts, 0);
        QCOMPARE(controller.topicSummarySettingsError(), QString("forbidden"));
    }

    void topicSummaryForeignStreamTopicTabIsUnknown()
    {
        TopicSummaryScene scene; scene.ownStream = false; QVERIFY(scene.fixture.start());
        QCOMPARE(scene.fixture.controller.topicSummaryTabPermission("topic"), QString("unknown"));
    }

    void topicSummaryEndpointCrudRoundTrip()
    {
        TopicSummaryScene scene; scene.permissions = {QJsonValue("workspace.topic_summary_endpoint.manage")};
        scene.endpointsResponse = QJsonArray{summaryEndpointRow()};
        QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
        controller.loadTopicSummaryEndpoints();
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(controller.topicSummaryEndpoints().size(), qsizetype(1));
        const auto row = controller.topicSummaryEndpoints().first().toMap();
        QCOMPARE(row.value("uuid").toString(), summaryEndpointUuid);
        QVERIFY(row.value("last_error_code").isNull()); // absent audit fields normalize to null
        // Create: uuid client-generated, defaults applied, trailing slash trimmed.
        const bool created = controller.createTopicSummaryEndpoint({{"name", " Prod "},
            {"base_url", "https://llm.example/v1/"}, {"model", "gpt-x"}, {"api_key", "***"}});
        QVERIFY(created); QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(scene.endpointPosts.size(), 1);
        const auto post = scene.endpointPosts.first();
        QVERIFY(isUuidText(post.value("uuid").toString()));
        QCOMPARE(post.value("name").toString(), QString("Prod"));
        QCOMPARE(post.value("base_url").toString(), QString("https://llm.example/v1"));
        QCOMPARE(post.value("priority").toInt(), 100); QCOMPARE(post.value("temperature").toDouble(), 0.2);
        QCOMPARE(post.value("max_output_tokens").toInt(), 512); QCOMPARE(post.value("top_p").toDouble(), 1.0);
        QCOMPARE(post.value("enabled").toBool(), true); QCOMPARE(post.value("supports_vision").toBool(), false);
        QCOMPARE(controller.topicSummaryEndpoints().size(), 2);
        // Partial update: only carried fields travel; an empty api_key is omitted.
        QVERIFY(controller.updateTopicSummaryEndpoint(summaryEndpointUuid,
            {{"model", "gpt-y"}, {"api_key", QString()}}));
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(scene.endpointPuts.size(), 1);
        QCOMPARE(scene.endpointPuts.first(), QJsonObject({{"model", "gpt-y"}}));
        QCOMPARE(controller.topicSummaryEndpoints().first().toMap().value("model").toString(), QString("gpt-y"));
        // An empty patch (only a blank api_key) is rejected before dispatch.
        const auto puts = scene.endpointPuts.size();
        QVERIFY(!controller.updateTopicSummaryEndpoint(summaryEndpointUuid, {{"api_key", QString()}}));
        QCOMPARE(scene.endpointPuts.size(), puts);
        QCOMPARE(controller.topicSummarySettingsError(), QString("invalid"));
        // Delete removes the row on success and leaves the created row.
        controller.deleteTopicSummaryEndpoint(summaryEndpointUuid);
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(scene.endpointDeletes, QStringList{summaryEndpointUuid});
        QCOMPARE(controller.topicSummaryEndpoints().size(), qsizetype(1));
        QCOMPARE(controller.topicSummaryEndpoints().first().toMap().value("uuid").toString(),
            post.value("uuid").toString());
    }

    void topicSummaryEndpointValidationBlocksDispatch_data()
    {
        QTest::addColumn<QVariantMap>("form"); QTest::addColumn<QString>("field");
        auto broken = [](const QString &name, const QVariant &value) {
            QVariantMap form{{"name", "Prod"}, {"base_url", "https://llm.example/v1"},
                {"model", "gpt-x"}, {"api_key", "***"}};
            form[name] = value; return form;
        };
        QTest::newRow("empty-name") << broken("name", "   ") << QString("name");
        QTest::newRow("long-name") << broken("name", QString(256, 'n')) << QString("name");
        QTest::newRow("userinfo-url") << broken("base_url", "https://user:***@llm.example/v1") << QString("base_url");
        QTest::newRow("query-url") << broken("base_url", "https://llm.example/v1?beta=1") << QString("base_url");
        QTest::newRow("fragment-url") << broken("base_url", "https://llm.example/v1#beta") << QString("base_url");
        QTest::newRow("ftp-url") << broken("base_url", "ftp://llm.example/v1") << QString("base_url");
        QTest::newRow("priority-high") << broken("priority", 1000001) << QString("priority");
        QTest::newRow("priority-string") << broken("priority", "5") << QString("priority");
        QTest::newRow("temperature-high") << broken("temperature", 2.5) << QString("temperature");
        QTest::newRow("tokens-zero") << broken("max_output_tokens", 0) << QString("max_output_tokens");
    }
    void topicSummaryEndpointValidationBlocksDispatch()
    {
        QFETCH(QVariantMap, form); QFETCH(QString, field);
        TopicSummaryScene scene; scene.permissions = {QJsonValue("workspace.topic_summary_endpoint.manage")};
        QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
        const auto errors = controller.topicSummaryEndpointValidationErrors(form, true);
        QVERIFY2(errors.contains(field), qPrintable(errors.join(',')));
        QVERIFY(!controller.createTopicSummaryEndpoint(form));
        QVERIFY(scene.endpointPosts.isEmpty());
        QCOMPARE(controller.topicSummarySettingsError(), QString("invalid"));
    }

    void topicSummaryEndpointSchemaMismatchStopsListParse()
    {
        TopicSummaryScene scene; scene.permissions = {QJsonValue("workspace.topic_summary_endpoint.manage")};
        auto row = summaryEndpointRow(); row["priority"] = "high";
        scene.endpointsResponse = QJsonArray{row};
        QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
        controller.loadTopicSummaryEndpoints();
        QTRY_VERIFY(!controller.topicSummarySettingsBusy());
        QCOMPARE(controller.topicSummarySettingsError(), QString("contract"));
        QVERIFY(controller.topicSummaryEndpoints().isEmpty());
    }

    // Issue #635: the dialog's topic tab save (docs/topic-summary-settings.md).
    // Reference surface: POST set_summary_prompt/invoke with AtLeastOne dirty-only
    // patch (workspace_ui e0194ff messenger.types.ts:435-439); empty prompt means
    // the app default and is sent as explicit null, effort "" the provider default.
    void topicSummaryTopicSavePatchesOnlyDirtyFields_data()
    {
        QTest::addColumn<QJsonObject>("rowConfig");
        QTest::addColumn<QVariantList>("saveArgs"); // enabled, prompt, effort
        QTest::addColumn<QJsonObject>("expectedBody"); // empty = must not dispatch
        {
            const QJsonObject row;
            const QVariantList args{true, QStringLiteral("Summarize decisions"), QString()};
            const QJsonObject expected{{"summary_system_prompt", "Summarize decisions"}};
            QTest::newRow("prompt-only") << row << args << expected;
        }
        {
            const QJsonObject row{{"summary_system_prompt", "Old prompt"}};
            const QVariantList args{true, QString(), QString()};
            const QJsonObject expected{{"summary_system_prompt", QJsonValue::Null}};
            QTest::newRow("prompt-to-default") << row << args << expected;
        }
        {
            const QJsonObject row{{"summary_system_prompt", "Keep"}};
            const QVariantList args{true, QStringLiteral("Keep"), QStringLiteral("high")};
            const QJsonObject expected{{"summary_reasoning_effort", "high"}};
            QTest::newRow("effort-only") << row << args << expected;
        }
        {
            const QJsonObject row{{"summary_enabled", false}, {"summary_reasoning_effort", "low"}};
            const QVariantList args{false, QString(), QStringLiteral("low")};
            const QJsonObject expected;
            QTest::newRow("nothing-dirty") << row << args << expected;
        }
        {
            const QJsonObject row{{"summary_reasoning_effort", "medium"}};
            const QVariantList args{false, QStringLiteral("New prompt"), QStringLiteral("medium")};
            const QJsonObject expected{{"summary_enabled", false}, {"summary_system_prompt", "New prompt"}};
            QTest::newRow("enabled-and-prompt") << row << args << expected;
        }
    }
    void topicSummaryTopicSavePatchesOnlyDirtyFields()
    {
        QFETCH(QJsonObject, rowConfig); QFETCH(QVariantList, saveArgs); QFETCH(QJsonObject, expectedBody);
        TopicSummaryScene scene; scene.topicSummaryConfig = rowConfig; scene.serveTopicRows(scene.fixture);
        QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
        // The mock echoes the patch onto the row DTO like the reference backend.
        const auto base = scene.fixture.server.handler;
        QList<QJsonObject> bodies;
        scene.fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path.endsWith("/actions/set_summary_prompt/invoke")) {
                bodies.append(request.body);
                auto dto = scene.topicRow(topicId);
                for (const auto key : request.body.keys()) dto.insert(key, request.body.value(key));
                return Response{dto};
            }
            return base(request);
        };
        QSignalSpy finished(&controller, &NativeController::channelTopicActionFinished);
        controller.saveTopicSummaryTopic(topicId, saveArgs.at(0).toBool(),
            saveArgs.at(1).toString(), saveArgs.at(2).toString());
        QTRY_COMPARE(finished.count(), 1);
        const auto signal = finished.takeFirst();
        QCOMPARE(signal.at(0).toString(), topicId);
        QCOMPARE(signal.at(1).toString(), QString("summary"));
        QCOMPARE(signal.at(2).toBool(), true);
        const QList<QJsonObject> expectedBodies =
            expectedBody.isEmpty() ? QList<QJsonObject>{} : QList<QJsonObject>{expectedBody};
        QCOMPARE(bodies, expectedBodies);
        if (!expectedBody.isEmpty() && expectedBody.contains("summary_system_prompt")) {
            // The echoed row lands in the drawer projection the dialog reads back.
            const QString want = expectedBody.value("summary_system_prompt").isNull()
                ? QString() : expectedBody.value("summary_system_prompt").toString();
            const auto rows = controller.channelInfoTopics();
            const auto row = std::find_if(rows.cbegin(), rows.cend(), [&](const QVariant &value) {
                return value.toMap().value("id").toString() == topicId; });
            QVERIFY(row != rows.cend());
            QCOMPARE(row->toMap().value("summarySystemPrompt").toString(), want);
        }
    }

    void topicSummaryTopicSaveRejectsInvalidInputAndMembers()
    {
        {
            TopicSummaryScene scene; scene.serveTopicRows(scene.fixture);
            QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
            const auto base = scene.fixture.server.handler;
            int invokes = 0;
            scene.fixture.server.handler = [&](const Request &request) -> Response {
                if (request.method == "POST" && request.target.path().endsWith("/actions/set_summary_prompt/invoke")) {
                    ++invokes;
                    return Response{scene.topicRow(topicId)}; // accepted row echo like the backend
                }
                return base(request);
            };
            QSignalSpy finished(&controller, &NativeController::channelTopicActionFinished);
            // effort outside the reference enum, whitespace-only custom draft
            // (reference promptEmpty), and the 16384 cap must all refuse without a request.
            controller.saveTopicSummaryTopic(topicId, true, QStringLiteral("ok"), QStringLiteral("ultra"));
            controller.saveTopicSummaryTopic(topicId, true, QStringLiteral("ok"), QStringLiteral("LOW"));
            controller.saveTopicSummaryTopic(topicId, true, QStringLiteral("   "), QString());
            controller.saveTopicSummaryTopic(topicId, true, QString(16385, QLatin1Char('p')), QString());
            QCOMPARE(finished.count(), 4);
            for (const auto &signal : {finished.at(0), finished.at(1), finished.at(2), finished.at(3)})
                QCOMPARE(signal.at(2).toBool(), false);
            QCOMPARE(invokes, 0);
            // A 16384-char prompt is still valid (boundary).
            controller.saveTopicSummaryTopic(topicId, true, QString(16384, QLatin1Char('p')), QString());
            QTRY_COMPARE(finished.count(), 5);
            QCOMPARE(finished.at(4).at(2).toBool(), true);
            QCOMPARE(invokes, 1);
        }
        {
            // A member cannot edit the topic summary: refused before dispatch.
            TopicSummaryScene scene; scene.role = QStringLiteral("member"); scene.serveTopicRows(scene.fixture);
            QVERIFY(scene.fixture.start()); auto &controller = scene.fixture.controller;
            const auto base = scene.fixture.server.handler;
            int invokes = 0;
            scene.fixture.server.handler = [&](const Request &request) -> Response {
                if (request.method == "POST" && request.target.path().endsWith("/actions/set_summary_prompt/invoke"))
                    ++invokes;
                return base(request);
            };
            QSignalSpy finished(&controller, &NativeController::channelTopicActionFinished);
            controller.saveTopicSummaryTopic(topicId, false, QStringLiteral("x"), QString());
            QCOMPARE(finished.count(), 1);
            QCOMPARE(finished.at(0).at(2).toBool(), false);
            QCOMPARE(invokes, 0);
        }
    }
};

QTEST_MAIN(ControllerChannelsTest)
#include "native-controller-channels-test.moc"
