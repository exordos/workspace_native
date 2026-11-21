#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"

class ControllerDirectChatTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void directChatNewPairUsesExactSourceBodyAndVerifiedAggregate()
    {
        DirectChatFixture test; auto &controller = test.fixture.controller;
        QVERIFY(!controller.canStartDirectChatWith(channelMemberA)); QVERIFY(test.fixture.start());
        QTRY_VERIFY(controller.directChatDirectoryVerified());
        QSignalSpy complete(&controller, &NativeController::directChatOpened);
        QVERIFY(controller.canStartDirectChatWith(channelMemberA));
        const auto users = test.usersGets, catalogs = test.streamGets, history = test.fixture.totalHistoryGets();
        controller.startDirectChat(channelMemberA); controller.startDirectChat(channelMemberA);
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(controller.directChatPhase(), "ready"); QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 2);
        QCOMPARE(test.topicGets, 1); QCOMPARE(test.streamGets - catalogs, 1); QCOMPARE(test.usersGets, users);
        QCOMPARE(test.bodies.first(), QJsonObject({{"name", "Direct"}, {"description", "Private workspace"},
            {"source_name", "native"}, {"direct_user_uuid", channelMemberA}}));
        QCOMPARE(controller.directChatForm().value("expectedStreamId").toString(), directId);
        QCOMPARE(controller.selectedStream(), directId); QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(controller.conversationKind(), "stream"); QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        QVERIFY(controller.isDirectConversation()); QCOMPARE(controller.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(controller.title(), "Alice Adams"); QCOMPARE(controller.topics().size(), 1);
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1);
    }

    void directChatExistingPairOpensWithoutPost_data()
    {
        QTest::addColumn<QString>("variation");
        for (const auto *entry : {"ordinary", "peer-created", "renamed", "flags", "closed-default"})
            QTest::newRow(entry) << QString(entry);
    }
    void directChatExistingPairOpensWithoutPost()
    {
        QFETCH(QString, variation); DirectChatFixture test; test.seed();
        if (variation == "peer-created") test.created["owner"] = channelMemberA;
        if (variation == "renamed") { test.created["name"] = "Existing renamed conversation"; test.created["description"] = "Actual metadata"; }
        if (variation == "flags") { test.created["invite_only"] = true; test.created["announce"] = true; }
        if (variation == "closed-default") test.defaultTopic["is_done"] = true;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA.toUpper()); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 0); QCOMPARE(test.gets, 1); QCOMPARE(controller.title(), "Alice Adams");
        for (const auto &row : controller.streams()) if (row.toMap().value("id").toString() == directId) {
            QVERIFY(row.toMap().value("isDirect").toBool()); QCOMPARE(row.toMap().value("peerId").toString(), channelMemberA);
            QCOMPARE(row.toMap().value("displayName").toString(), "Alice Adams");
        }
        controller.selectTopic(channelDefault); QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.title(), "Alice Adams"); controller.setDraft("Actual topic draft");
        QCOMPARE(controller.canSend(), variation != "closed-default");
        QCOMPARE(controller.selectedTopic(), channelDefault); QVERIFY(controller.isDirectConversation());
    }

    void directChatDirectoryAndIdentityRejectUntrustedEntry_data()
    {
        QTest::addColumn<QString>("entry");
        for (const auto *entry : {"self", "foreign", "invalid", "empty", "unverified", "before-login"}) QTest::newRow(entry) << QString(entry);
    }
    void directChatDirectoryAndIdentityRejectUntrustedEntry()
    {
        QFETCH(QString, entry); DirectChatFixture test; auto &controller = test.fixture.controller;
        if (entry != "before-login") QVERIFY(test.fixture.start());
        if (entry == "unverified") { test.fixture.users = [](const Request &) { return Response{QJsonObject{}, 503}; }; controller.refresh(); QTRY_VERIFY(!controller.busy()); }
        const auto peer = entry == "self" ? userId : entry == "foreign" ? returnedId : entry == "invalid" ? "not-a-uuid"
            : entry == "empty" ? QString{} : channelMemberA;
        QVERIFY(!controller.canStartDirectChatWith(peer)); controller.startDirectChat(peer);
        QCOMPARE(test.gets, 0); QCOMPARE(test.posts, 0); QVERIFY(controller.directChatForm().isEmpty());
    }

    void directChatLookupCannotTurnInvalidIdentityIntoCreateAuthority_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"401", "403", "500", "uuid", "project_id", "user_uuid", "owner", "direct_user_uuid",
             "private", "source_name", "provider_uuid", "role", "encryption", "invalid-current-key",
             "nonencrypted-current-key", "created_at", "description", "default_topic_uuid"})
            QTest::newRow(field) << QString(field);
    }
    void directChatLookupCannotTurnInvalidIdentityIntoCreateAuthority()
    {
        QFETCH(QString, field); DirectChatFixture test; test.seed();
        if (field == "401" || field == "403" || field == "500") test.lookupStatus = field.toInt();
        else if (field == "private") test.created[field] = false;
        else if (field == "encryption") test.created[field] = "true"; // Malformed type; encrypted+null is now a valid pair.
        else if (field == "invalid-current-key") { test.created["encryption"] = true; test.created["current_encryption_key_uuid"] = "not-a-uuid"; }
        else if (field == "nonencrypted-current-key") test.created["current_encryption_key_uuid"] = returnedId;
        else if (field == "source_name") test.created[field] = "zulip";
        else if (field == "role") test.created[field] = "member";
        else if (field == "created_at") test.created[field] = "invalid-date";
        else if (field == "description") test.created[field] = 1;
        else test.created[field] = returnedId;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(!controller.directChatBusy());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 0); QVERIFY(controller.selectedStream() != directId);
        if (field != "500" && field != "default_topic_uuid") QVERIFY(!controller.canRetryDirectChat());
        controller.retryDirectChat(); QTRY_VERIFY(!controller.directChatBusy()); QCOMPARE(test.posts, 0);
    }

    void directChatUnsupportedExistingPairNeverDowngradesOrUnarchives_data()
    {
        // Valid encrypted pairs are covered by the positive known/null-current table below.
        QTest::addColumn<bool>("encrypted"); QTest::newRow("archived") << false;
    }
    void directChatUnsupportedExistingPairNeverDowngradesOrUnarchives()
    {
        QFETCH(bool, encrypted); DirectChatFixture test; test.seed();
        if (encrypted) { test.created["encryption"] = true; test.created["current_encryption_key_uuid"] = returnedId; }
        else test.created["is_archived"] = true;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(controller.directChatPhase(), "unsupported");
        QCOMPARE(test.posts, 0); QCOMPARE(test.topicGets, 0); QVERIFY(!controller.canCheckDirectChat()); QVERIFY(!controller.canRetryDirectChat());
        QCOMPARE(controller.directChatForm().value("confirmedStreamId").toString(), directId);
        QVERIFY(controller.selectedStream() != directId);
    }

    void directChatEncryptedExistingPairPreservesCanonicalMetadata_data()
    {
        QTest::addColumn<bool>("currentKeyKnown"); QTest::addColumn<bool>("peerCreated");
        QTest::newRow("known-current-own-created") << true << false;
        QTest::newRow("null-current-own-created") << false << false;
        QTest::newRow("known-current-peer-created") << true << true;
        QTest::newRow("null-current-peer-created") << false << true;
    }
    void directChatEncryptedExistingPairPreservesCanonicalMetadata()
    {
        QFETCH(bool, currentKeyKnown); QFETCH(bool, peerCreated);
        DirectChatFixture test;
        auto pair = creationDirect(); pair["encryption"] = true;
        pair["current_encryption_key_uuid"] = currentKeyKnown ? QJsonValue(returnedId) : QJsonValue(QJsonValue::Null);
        if (peerCreated) pair["owner"] = channelMemberA;
        test.seed(pair); const auto originalPair = test.created, originalTopic = test.defaultTopic;
        auto &controller = test.fixture.controller;
        FileClient files(&test.fixture.api); controller.useFiles(&files);
        QVERIFY(test.fixture.start()); QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto catalogs = test.streamGets, users = test.usersGets;
        QSignalSpy done(&controller, &NativeController::directChatOpened); QVERIFY(done.isValid());
        controller.startDirectChat(channelMemberA);
        QTRY_COMPARE(done.size(), 1); QCOMPARE(controller.directChatPhase(), QString("ready"));
        QCOMPARE(test.gets, 1); QCOMPARE(test.posts, 0); QCOMPARE(test.topicGets, 1);
        QCOMPARE(test.streamGets - catalogs, 1); QCOMPARE(test.usersGets, users);
        QCOMPARE(done.first().at(0).toString(), directId);
        QCOMPARE(done.first().at(1).toString(), channelMemberA);
        QCOMPARE(done.first().at(2).toString(), channelDefault);
        QCOMPARE(controller.selectedStream(), directId); QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(controller.conversationKind(), QString("stream")); QVERIFY(controller.isDirectConversation());
        QCOMPARE(controller.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(test.created, originalPair); QCOMPARE(test.defaultTopic, originalTopic);
        QCOMPARE(controller.encryption().value("currentKeyUuid").toString(), currentKeyKnown ? returnedId : QString{});
        controller.selectTopic(channelDefault); QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedTopic(), channelDefault);
        controller.setDraft("Must stay local without a Vault key");
        QVERIFY(!controller.encryption().value("available").toBool());
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        controller.send(); controller.attachFiles({QUrl::fromLocalFile("/nonexistent/cassi-encrypted-direct-no-upload")});
        QCOMPARE(controller.draft(), QString("Must stay local without a Vault key"));
        for (const auto &request : test.fixture.server.requests)
            if (request.target.path().startsWith("/api/workspace/v1/messenger/")) QCOMPARE(request.method, QByteArray("GET"));
        QCOMPARE(test.posts, 0); QCOMPARE(test.created, originalPair); QCOMPARE(test.defaultTopic, originalTopic);
    }

    void directChatOrdinaryPostMayReturnEncryptedExistingPair_data()
    {
        QTest::addColumn<bool>("currentKeyKnown");
        QTest::newRow("known-current") << true; QTest::newRow("null-current") << false;
    }
    void directChatOrdinaryPostMayReturnEncryptedExistingPair()
    {
        QFETCH(bool, currentKeyKnown); DirectChatFixture test;
        // A pair may appear between the initial404 and the idempotent ordinary
        // create. The POST body still never requests encryption or bootstrap.
        test.alterPost = [&](QJsonObject &row) {
            row["encryption"] = true;
            row["current_encryption_key_uuid"] = currentKeyKnown ? QJsonValue(returnedId) : QJsonValue(QJsonValue::Null);
            test.created = row; // The subsequent actual GET/catalog return the same canonical encrypted pair.
        };
        auto &controller = test.fixture.controller;
        FileClient files(&test.fixture.api); controller.useFiles(&files);
        QVERIFY(test.fixture.start()); QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto catalogs = test.streamGets;
        QSignalSpy done(&controller, &NativeController::directChatOpened); QVERIFY(done.isValid());
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(controller.directChatPhase(), QString("ready"));
        QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 2); QCOMPARE(test.topicGets, 1);
        QCOMPARE(test.streamGets - catalogs, 1); QCOMPARE(test.bodies.size(), 1);
        QCOMPARE(test.bodies.first(), QJsonObject({{"name", "Direct"}, {"description", "Private workspace"},
            {"source_name", "native"}, {"direct_user_uuid", channelMemberA}}));
        QCOMPARE(controller.selectedStream(), directId); QVERIFY(controller.selectedTopic().isEmpty());
        QVERIFY(test.created.value("encryption").toBool());
        QCOMPARE(test.created.value("current_encryption_key_uuid"),
            currentKeyKnown ? QJsonValue(returnedId) : QJsonValue(QJsonValue::Null));
        int pairPosts = 0;
        for (const auto &request : test.fixture.server.requests) {
            if (!request.target.path().startsWith("/api/workspace/v1/messenger/")) continue;
            if (request.method != "GET") {
                QCOMPARE(request.method, QByteArray("POST"));
                QCOMPARE(request.target.path(), QString("/api/workspace/v1/messenger/streams/"));
                ++pairPosts;
            }
        }
        QCOMPARE(pairPosts, 1); // No member write, key request, bootstrap or message POST.
        controller.selectTopic(channelDefault); QTRY_VERIFY(!controller.busy());
        controller.setDraft("No plaintext fallback after an encrypted canonical result");
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        const auto requestsBeforeSend = test.fixture.server.requests.size(); controller.send();
        QCOMPARE(test.fixture.server.requests.size(), requestsBeforeSend);
        QCOMPARE(controller.draft(), QString("No plaintext fallback after an encrypted canonical result"));
    }

    void directChatUnknownPostRecoversOnlyThroughExplicitActions_data()
    {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("applied503") << QString("applied503"); QTest::newRow("not-applied503") << QString("not-applied503");
        QTest::newRow("malformed201") << QString("malformed201");
    }
    void directChatUnknownPostRecoversOnlyThroughExplicitActions()
    {
        QFETCH(QString, outcome); DirectChatFixture test; test.postStatus = outcome == "malformed201" ? 201 : 503;
        test.applyPost = outcome != "not-applied503";
        if (outcome == "malformed201") test.alterPost = [](QJsonObject &row) { row["uuid"] = returnedId; };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(controller.directChatUncertain());
        const auto form = controller.directChatForm(); QCOMPARE(test.posts, 1);
        controller.checkDirectChat(); QTRY_VERIFY(!controller.directChatBusy()); QCOMPARE(test.posts, 1);
        if (test.applyPost) { QCOMPARE(done.size(), 1); return; }
        QVERIFY(controller.directChatUncertain()); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY(!controller.directChatBusy()); QCOMPARE(test.posts, 1); QVERIFY(controller.canRetryDirectChat());
        QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        test.postStatus = 201; test.applyPost = true; controller.retryDirectChat(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 2); QCOMPARE(test.bodies[0], test.bodies[1]);
        QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        QCOMPARE(controller.directChatForm().value("expectedStreamId").toString(), directId);
    }

    void directChatDefinitePostFailureAllowsOnlyANewExplicitAttempt_data()
    {
        QTest::addColumn<int>("status"); for (int status : {400, 401, 403, 404}) QTest::newRow(qPrintable(QString::number(status))) << status;
    }
    void directChatDefinitePostFailureAllowsOnlyANewExplicitAttempt()
    {
        QFETCH(int, status); DirectChatFixture test; test.postStatus = status; test.applyPost = false;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(controller.directChatPhase(), "failed");
        const auto form = controller.directChatForm(); QCOMPARE(test.posts, 1); QVERIFY(!controller.canRetryDirectChat());
        controller.checkDirectChat(); controller.retryDirectChat(); QCOMPARE(test.posts, 1);
        test.postStatus = 201; test.applyPost = true; controller.startDirectChat(channelMemberA);
        QTRY_COMPARE(controller.directChatPhase(), "ready"); QCOMPARE(test.posts, 2);
        QVERIFY(controller.directChatForm().value("attemptId") != form.value("attemptId"));
        QCOMPARE(controller.directChatForm().value("expectedStreamId"), form.value("expectedStreamId"));
    }

    void directChatAndChannelShareOneMutationAdmissionGate()
    {
        DirectChatFixture direct; QVERIFY(direct.fixture.start()); auto &controller = direct.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); direct.postDelay = 150;
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(direct.posts, 1);
        QVERIFY(!controller.canCreateChannel()); controller.createChannel("Do not overlap", "", false, false, {});
        QVERIFY(controller.channelCreationForm().isEmpty()); QTRY_COMPARE(controller.directChatPhase(), "ready");
        ChannelCreationFixture channel; QVERIFY(channel.fixture.start()); QTRY_VERIFY(channel.fixture.controller.channelMembersVerified());
        channel.postDelay = 150; channel.fixture.controller.createChannel("One flow", "", false, false, {}); QTRY_COMPARE(channel.posts, 1);
        QVERIFY(!channel.fixture.controller.canStartDirectChatWith(channelMemberA));
        channel.fixture.controller.startDirectChat(channelMemberA); QVERIFY(channel.fixture.controller.directChatForm().isEmpty());
        QTRY_COMPARE(channel.fixture.controller.channelCreationPhase(), "ready");
    }

    void directChatPublicationCannotCompleteAReentrantReplacementIntent()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        bool replaced = false; QVariantMap replacement;
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (replaced || controller.selectedStream() != directId || controller.directChatBusy()) return;
            replaced = true; test.lookupStatus = 503; test.getDelay = 150;
            controller.startDirectChat(channelMemberB); replacement = controller.directChatForm();
        });
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(replaced);
        QCOMPARE(controller.directChatForm().value("peerUuid").toString(), channelMemberB);
        QCOMPARE(controller.directChatPhase(), "looking-up"); QVERIFY(controller.directChatBusy());
        QCOMPARE(controller.directChatForm().value("attemptId"), replacement.value("attemptId"));
        QTRY_VERIFY(!controller.directChatBusy()); QVERIFY(controller.directChatUncertain()); QCOMPARE(done.size(), 0);
        QCOMPARE(test.posts, 1);
    }

    void directChatReadEventsCannotPublishTheEarlierSavedTopicsSnapshot()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); test.seed();
        test.defaultTopic["unread_count"] = 7; test.defaultTopic["active_unread_count"] = 7;
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        bool read = false; QSignalSpy done(&controller, &NativeController::directChatOpened);
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (read || controller.directChatPhase() != "refreshing-catalogs") return;
            read = true; test.defaultTopic["unread_count"] = 0; test.defaultTopic["active_unread_count"] = 0;
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.read", test.defaultTopic)));
        });
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1); QVERIFY(read);
        QCOMPARE(controller.topics().first().toMap().value("unread").toLongLong(), 0);
        QCOMPARE(test.posts, 0); QVERIFY(test.topicGets >= 2); QVERIFY(test.topicGets <= 4);
    }

    void directChatCanonicalPairMatchesBackendVectorsAndReciprocalViewer()
    {
        QCOMPARE(expectedDirectId(channelMemberA), directId);
        QCOMPARE(expectedDirectId(userId, channelMemberA), directId);
        QCOMPARE(expectedDirectId(channelMemberA, userId, returnedId), QString("c002ebce-130a-571f-ab1d-9dc19b58dd7f"));
        for (const auto &identity : {qMakePair(channelMemberA, userId), qMakePair(userId, returnedId)}) {
            DirectChatFixture test; const auto viewer = identity.first;
            const auto project = identity.second == userId ? projectId : returnedId;
            const auto peer = identity.second == userId ? userId : channelMemberA;
            const auto expected = expectedDirectId(peer, viewer, project);
            const auto fallback = test.fixture.server.handler;
            test.fixture.server.handler = [&, fallback](const Request &request) {
                auto response = fallback(request); const auto path = request.target.path();
                if (path.endsWith("/actions/introspect")) {
                    auto row = response.data.toObject(); row["project_id"] = project; row["user_info"] = QJsonObject{{"uuid", viewer}}; response.data = row;
                } else if (path == "/api/core/v1/iam/projects/") response.data = QJsonArray{QJsonObject{{"uuid", project}, {"name", "Project"}, {"status", "ACTIVE"}}};
                else if (path.endsWith("/streams/") && request.method == "GET") {
                    auto row = creationChannel(streamId); row["project_id"] = project; row["user_uuid"] = viewer; response.data = QJsonArray{row};
                } else if (path.endsWith("/users/")) response.data = QJsonArray{QJsonObject{{"uuid", peer}, {"username", "peer"}}};
                else if (path == "/api/workspace/v1/messenger/streams/" + expected) response = {QJsonObject{}, 503};
                return response;
            };
            auto &controller = test.fixture.controller;
            controller.login(test.fixture.server.origin().toString(), "synthetic", "synthetic", "");
            QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(project);
            QTRY_VERIFY(controller.directChatDirectoryVerified()); QTRY_VERIFY(controller.canStartDirectChatWith(peer));
            controller.startDirectChat(peer); QTRY_VERIFY(!controller.directChatBusy());
            QCOMPARE(controller.directChatForm().value("expectedStreamId").toString(), expected); QCOMPARE(test.posts, 0);
        }
    }

    void directChatContextLossFencesEveryDelayedStage_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("change");
        for (const auto *stage : {"lookup", "post", "canonical", "default", "catalog"})
            for (const auto *change : {"route", "logout", "token", "snapshot", "cache-replace", "cache-destroy"})
                QTest::newRow(qPrintable(QString(stage) + ":" + change)) << QString(stage) << QString(change);
    }
    void directChatContextLossFencesEveryDelayedStage()
    {
        QFETCH(QString, stage); QFETCH(QString, change); DirectChatFixture test;
        QTemporaryDir first, second;
        auto cache = std::make_unique<NativeCacheService>(first.path(), NativeCache::Policy{}, true);
        NativeCacheService replacement(second.path(), {}, true);
        if (change.startsWith("cache-")) test.fixture.controller.useCache(cache.get());
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; bool delayed = false;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            const bool target = (stage == "lookup" && path.endsWith(directId) && test.gets == 1)
                || (stage == "post" && request.method == "POST" && path.endsWith("/streams/"))
                || (stage == "canonical" && path.endsWith(directId) && test.gets == 2)
                || (stage == "default" && path.endsWith("/stream_topics/") && !test.created.isEmpty())
                || (stage == "catalog" && path.endsWith("/streams/") && request.method == "GET" && !test.created.isEmpty());
            if (target && !delayed) { delayed = true; result.delay = 150; }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY(delayed); const auto form = controller.directChatForm(); const auto key = controller.directChatContextKey();
        if (change == "route") controller.selectStream({});
        else if (change == "logout") controller.logout();
        else if (change == "cache-replace") controller.useCache(&replacement);
        else if (change == "cache-destroy") cache.reset();
        else if (change == "snapshot") controller.reloadSnapshot();
        else {
            test.fixture.authentication = [](const Request &request) {
                if (request.target.path().endsWith("/actions/get_token/invoke")) return Response{QJsonObject{
                    {"access_token", "rotated-direct-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
                return sessionResponse(request);
            };
            test.fixture.session.requestRefresh();
        }
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, stage == "lookup" ? 0 : 1);
        QVERIFY(!controller.directChatBusy()); QVERIFY(controller.selectedStream() != directId);
        QVERIFY(controller.directChatContextKey() != key);
        if (change == "logout") QVERIFY(controller.directChatForm().isEmpty());
        else { QVERIFY(controller.directChatUncertain()); QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId")); }
    }

    void directChatOwnerAndProjectChangesCannotPublishAnOlderAttempt_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<bool>("projectChange");
        for (const auto *stage : {"lookup", "post", "canonical", "default", "catalog"})
            for (bool project : {false, true}) QTest::newRow(qPrintable(QString(stage) + (project ? ":project" : ":owner"))) << QString(stage) << project;
    }
    void directChatOwnerAndProjectChangesCannotPublishAnOlderAttempt()
    {
        QFETCH(QString, stage); QFETCH(bool, projectChange); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; bool delayed = false;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            if (!delayed && ((stage == "lookup" && path.endsWith(directId) && test.gets == 1)
                || (stage == "post" && request.method == "POST" && path.endsWith("/streams/"))
                || (stage == "canonical" && path.endsWith(directId) && test.gets == 2)
                || (stage == "default" && path.endsWith("/stream_topics/") && !test.created.isEmpty())
                || (stage == "catalog" && path.endsWith("/streams/") && request.method == "GET" && !test.created.isEmpty()))) {
                delayed = true; result.delay = 150;
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_VERIFY(delayed);
        const auto form = controller.directChatForm(); const auto newProject = projectChange ? returnedId : projectId;
        const auto newUser = projectChange ? userId : returnedId;
        test.fixture.server.handler = [fallback, newProject, newUser](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            if (path.endsWith("/actions/introspect")) {
                auto row = result.data.toObject(); row["project_id"] = newProject; row["user_info"] = QJsonObject{{"uuid", newUser}}; result.data = row;
            } else if (path == "/api/core/v1/iam/projects/") result.data = QJsonArray{QJsonObject{{"uuid", newProject}, {"name", "Other"}, {"status", "ACTIVE"}}};
            else if (path.endsWith("/streams/") && request.method == "GET") {
                auto row = creationChannel(streamId); row["project_id"] = newProject; row["user_uuid"] = newUser; row["owner"] = newUser;
                result.data = QJsonArray{row}; result.delay = 0;
            }
            return result;
        };
        controller.login(test.fixture.server.origin().toString(), "another-synthetic", "synthetic", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(newProject);
        QTRY_VERIFY(controller.authenticated()); QTRY_COMPARE(controller.streams().size(), 1);
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QVERIFY(controller.directChatForm().isEmpty());
        QCOMPARE(test.posts, stage == "lookup" ? 0 : 1);
        test.fixture.server.handler = fallback;
        controller.login(test.fixture.server.origin().toString(), "synthetic", "synthetic", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId); QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QTRY_VERIFY(controller.canCheckDirectChat());
        QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        controller.checkDirectChat(); QTRY_VERIFY(!controller.directChatBusy());
        QCOMPARE(test.posts, stage == "lookup" ? 0 : 1);
        QCOMPARE(done.size(), stage == "lookup" ? 0 : 1);
    }

    void directChatUnsafeTargetChangesAndAbaCannotPublishAnEarlierSnapshot_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"stream-delete", "default-delete", "default-replace", "binding-delete", "binding-role", "private-aba", "encrypted-aba", "topic-aba", "late-topic-update"})
            QTest::newRow(change) << QString(change);
    }
    void directChatUnsafeTargetChangesAndAbaCannotPublishAnEarlierSnapshot()
    {
        QFETCH(QString, change); DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        test.catalogDelay = 150; const auto catalogs = test.streamGets;
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(test.streamGets, catalogs + 1);
        const auto key = controller.directChatContextKey(); const auto form = controller.directChatForm();
        if (change == "stream-delete") QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.deleted", QJsonObject{{"uuid", directId}})));
        else if (change == "default-delete") QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", channelDefault}, {"stream_uuid", directId}})));
        else if (change.startsWith("binding")) {
            auto row = creationBinding(userId, 900); row["stream_uuid"] = directId; row["role"] = "member";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent(change == "binding-delete" ? "stream_binding.deleted" : "stream_binding.updated", row)));
        } else if (change == "topic-aba" || change == "late-topic-update") {
            auto topic = test.defaultTopic; topic["name"] = "A newer topic";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", topic)));
            if (change == "topic-aba") QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", test.defaultTopic, 13)));
            else test.defaultTopic = topic;
        } else {
            auto row = test.created;
            if (change == "private-aba") row["private"] = false;
            else if (change == "encrypted-aba") { row["encryption"] = true; row["current_encryption_key_uuid"] = returnedId; }
            else row["default_topic_uuid"] = returnedId;
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
            if (change.endsWith("aba")) QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", test.created, 13)));
        }
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QVERIFY(controller.directChatUncertain());
        QVERIFY(controller.selectedStream() != directId); QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        if (change != "topic-aba" && change != "late-topic-update") { QVERIFY(controller.directChatContextKey() != key); QVERIFY(!controller.canRetryDirectChat()); }
        test.catalogDelay = 0; controller.checkDirectChat(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
        if (change == "late-topic-update") QCOMPARE(controller.topics().first().toMap().value("name").toString(), "A newer topic");
    }

    void directChatMatchingOwnEventsUseOnlyBoundedReadReconciliation()
    {
        DirectChatFixture test; RealtimeGateway gateway(test.fixture.server.serverPort()); QVector<QJsonObject> events;
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
        test.getDelay = test.topicDelay = test.catalogDelay = 70;
        const auto fallback = test.fixture.server.handler; QSet<QString> sent;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request); if (test.created.isEmpty()) return result;
            const auto path = request.target.path(); QString kind; QJsonObject payload;
            if (request.method == "GET" && path.endsWith(directId)) { kind = "stream.created"; payload = test.created; }
            else if (path.endsWith("/stream_topics/")) { kind = "topic.created"; payload = test.defaultTopic; }
            else if (request.method == "GET" && path.endsWith("/streams/")) {
                kind = "stream_bindings.created"; auto first = creationBinding(userId, 900), second = creationBinding(channelMemberA, 901);
                for (auto *row : {&first, &second}) { (*row)["stream_uuid"] = directId; (*row)["role"] = "owner"; }
                payload = {{"uuid", directId}, {"items", QJsonArray{first, second}}};
            }
            if (!kind.isEmpty() && !sent.contains(kind)) {
                sent.insert(kind); const auto event = controllerEvent(kind, payload, 10 + sent.size()); events.append(event);
                QTimer::singleShot(0, &controller, [&gateway, event] { gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact))); });
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY2(done.size() == 1, qPrintable(controller.directChatPhase() + ": " + controller.directChatError()
            + QString(" gets=%1 topics=%2 catalogs=%3 events=%4 ack=%5").arg(test.gets).arg(test.topicGets).arg(test.streamGets).arg(sent.size()).arg(realtime.acknowledgedEpoch())));
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(13));
        QCOMPARE(test.posts, 1); QVERIFY(test.gets <= 5); QVERIFY(test.topicGets <= 4); QVERIFY(test.streamGets <= 5);
        QCOMPARE(controller.directChatPhase(), "ready"); QCOMPARE(sent.size(), 3);
    }

    void directChatReconciliationExhaustionKeepsExplicitGetOnlyRecovery()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); const auto fallback = test.fixture.server.handler; int interleavings = 0;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request);
            if (!test.created.isEmpty() && request.target.path().endsWith(directId) && interleavings < 4) {
                ++interleavings; result.delay = 40;
                QTimer::singleShot(0, &controller, [&] { QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.created", test.created, 20 + interleavings))); });
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY(controller.directChatUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QCOMPARE(interleavings, 4);
        controller.checkDirectChat(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
    }

    void directChatBenignDirectoryAndCounterChangesKeepContextAndPeerIdentity()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); test.getDelay = 150;
        controller.startDirectChat(channelMemberA); const auto form = controller.directChatForm(); const auto key = controller.directChatContextKey();
        auto existing = creationChannel(streamId); existing["unread_count"] = 7;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.read", existing)));
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", channelMemberA}, {"username", "alice"}, {"first_name", "Updated"}})));
        QCOMPARE(controller.directChatContextKey(), key); QCOMPARE(controller.directChatForm().value("peerUuid"), form.value("peerUuid"));
        QTRY_COMPARE(controller.directChatPhase(), "ready"); QCOMPARE(controller.title(), "Updated"); QCOMPARE(test.posts, 1);
    }

    void directChatCatalogCacheSurvivesColdFailureAndWarmNavigationAvoidsExtraGet()
    {
        QTemporaryDir directory; DirectChatFixture test; QVERIFY(test.fixture.start());
        {
            NativeCacheService cache(directory.path(), {}, true); NativeController controller(&test.fixture.api, &test.fixture.session);
            controller.useCache(&cache); QTRY_VERIFY(controller.directChatDirectoryVerified()); QTRY_VERIFY(controller.canStartDirectChatWith(channelMemberA));
            QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
            QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache));
            auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}); QVERIFY(saved);
            bool found = false; for (const auto &value : saved->rows) if (value.toObject().value("uuid").toString() == directId) {
                found = true; QVERIFY(value.toObject().value("private").toBool()); QCOMPARE(value.toObject().value("direct_user_uuid").toString(), channelMemberA);
            }
            QVERIFY(found); controller.selectStream(streamId); QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache));
            const auto topics = test.topicGets, catalogs = test.streamGets, users = test.usersGets, history = test.fixture.totalHistoryGets();
            controller.selectStream(directId); QTRY_VERIFY(!controller.busy()); QTRY_COMPARE(controller.topics().size(), 1);
            QCOMPARE(test.topicGets, topics); QCOMPARE(test.streamGets, catalogs); QCOMPARE(test.usersGets, users); QCOMPARE(test.fixture.totalHistoryGets(), history);
            QVERIFY(flushCache(cache));
        }
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback](const Request &request) {
            if (request.method == "GET" && (request.target.path().contains("/messenger/") || request.target.path().endsWith("/users/"))) return Response{QJsonObject{}, 503};
            return fallback(request);
        };
        NativeCacheService reopened(directory.path(), {}, true); NativeController restored(&test.fixture.api, &test.fixture.session);
        restored.useCache(&reopened); QTRY_COMPARE(restored.streams().size(), 2); QVERIFY(restored.cacheStale());
        restored.selectStream(directId); QTRY_COMPARE(restored.topics().size(), 1);
        QVERIFY(restored.isDirectConversation()); QCOMPARE(restored.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(restored.title(), "Alice Adams"); QVERIFY(!restored.canStartDirectChatWith(channelMemberA));
        QVERIFY(!restored.canSend()); QVERIFY(!restored.canAttach()); QCOMPARE(test.posts, 1);
    }

    void directChatSubscribersMayDestroyControllerAtEveryBoundary_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const auto *boundary : {"looking-up", "creating", "verifying-stream", "verifying-default", "refreshing-catalogs", "streams", "topics", "opened"})
            QTest::newRow(boundary) << QString(boundary);
    }
    void directChatSubscribersMayDestroyControllerAtEveryBoundary()
    {
        QFETCH(QString, boundary); DirectChatFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> controller = new NativeController(&test.fixture.api, &test.fixture.session);
        const auto cleanup = qScopeGuard([&] { delete controller; });
        controller->refresh();
        QTRY_VERIFY(controller && controller->canStartDirectChatWith(channelMemberA));
        connect(controller, &NativeController::stateChanged, &test.fixture.controller, [&] {
            if (controller && controller->directChatPhase() == boundary) delete controller;
        });
        connect(controller, &NativeController::streamsChanged, &test.fixture.controller, [&] {
            if (controller && boundary == "streams" && test.gets >= 2) delete controller;
        });
        connect(controller, &NativeController::topicsChanged, &test.fixture.controller, [&] {
            if (controller && boundary == "topics" && controller->selectedStream() == directId) delete controller;
        });
        connect(controller, &NativeController::directChatOpened, &test.fixture.controller, [&] { if (controller && boundary == "opened") delete controller; });
        controller->startDirectChat(channelMemberA); QTRY_VERIFY(controller.isNull());
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(test.posts <= 1);
    }

    void directChatCollectionsAreBoundedAndValidateCompleteDtos_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("corruption");
        for (const auto *stage : {"default", "catalog"})
            for (const auto *corruption : {"page-cap", "marker-loop", "rows", "bytes", "duplicate", "viewer", "missing-target", "malformed"})
                QTest::newRow(qPrintable(QString(stage) + ":" + corruption)) << QString(stage) << QString(corruption);
    }
    void directChatCollectionsAreBoundedAndValidateCompleteDtos()
    {
        QFETCH(QString, stage); QFETCH(QString, corruption); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; int pages = 0;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request);
            if (test.created.isEmpty() || request.method != "GET" || !(stage == "default"
                ? request.target.path().endsWith("/stream_topics/") : request.target.path().endsWith("/streams/"))) return result;
            ++pages;
            if (corruption == "page-cap" || corruption == "marker-loop") {
                result.data = QJsonArray{};
                result.headers["X-Pagination-Marker"] = corruption == "marker-loop" ? "repeated" : QByteArray::number(pages);
            } else if (corruption == "rows") {
                QJsonArray rows; for (int i = 0; i < 1000; ++i) rows.append(QJsonObject{{"uuid", userId}});
                result.data = rows; result.headers["X-Pagination-Marker"] = QByteArray::number(pages);
            } else if (corruption == "bytes") {
                result.data = QJsonArray{QJsonObject{{"content", QString(8 * 1024 * 1024, 'x')}}};
                result.headers["X-Pagination-Marker"] = QByteArray::number(pages);
            } else if (corruption == "malformed") result.data = QJsonObject{};
            else {
                auto rows = result.data.toArray();
                if (corruption == "duplicate") rows.append(rows.last());
                else if (corruption == "missing-target") rows = QJsonArray{};
                else { auto row = rows.last().toObject(); row["user_uuid"] = returnedId; rows[rows.size() - 1] = row; }
                result.data = rows;
            }
            return result;
        };
        QSignalSpy changed(&controller, &NativeController::stateChanged), done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA);
        while (controller.directChatBusy()) QVERIFY(changed.wait());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QVERIFY(controller.directChatUncertain());
        QVERIFY(pages <= 500); if (corruption == "page-cap") QVERIFY(pages >= 499);
        if (corruption == "marker-loop") QCOMPARE(pages, 2);
        if (corruption == "rows") QCOMPARE(pages, stage == "default" ? 101 : 100);
        if (corruption == "bytes") QCOMPARE(pages, 4);
    }

    void directChatPagedPublicationKeepsExactNavigationRequestCounts_data()
    {
        QTest::addColumn<QString>("stage"); QTest::newRow("default") << QString("default"); QTest::newRow("catalog") << QString("catalog");
    }
    void directChatPagedPublicationKeepsExactNavigationRequestCounts()
    {
        QFETCH(QString, stage); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; int pages = 0; bool correctMarker = true;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request);
            if (!test.created.isEmpty() && request.method == "GET" && (stage == "default"
                ? request.target.path().endsWith("/stream_topics/") : request.target.path().endsWith("/streams/"))) {
                ++pages; const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
                if (marker.isEmpty()) { result.data = QJsonArray{}; result.headers["X-Pagination-Marker"] = "valid-next"; }
                else correctMarker &= marker == "valid-next";
            }
            return result;
        };
        const auto users = test.usersGets, catalogs = test.streamGets, history = test.fixture.totalHistoryGets();
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(pages, 2); QVERIFY(correctMarker); QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 2); QCOMPARE(test.usersGets, users);
        QCOMPARE(test.topicGets, stage == "default" ? 2 : 1); QCOMPARE(test.streamGets - catalogs, stage == "catalog" ? 2 : 1);
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1);
    }

    void directChatUncertainOtherPeerReplacementFencesLateFirstOwnerCredit()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); test.postDelay = 150;
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_COMPARE(test.posts, 1);
        const auto first = controller.directChatForm(); controller.selectStream({});
        QVERIFY(controller.directChatUncertain()); QVERIFY(controller.canStartDirectChatWith(channelMemberB));
        test.created = {}; test.postDelay = 0; controller.startDirectChat(channelMemberB);
        QTRY_COMPARE(done.size(), 1); QCOMPARE(controller.directChatForm().value("peerUuid").toString(), channelMemberB);
        QVERIFY(controller.directChatForm().value("attemptId") != first.value("attemptId"));
        QCOMPARE(controller.selectedStream(), expectedDirectId(channelMemberB));
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(done.size(), 1); QCOMPARE(test.posts, 2);
        QCOMPARE(done.first()[1].toString(), channelMemberB);
    }

    void directChatActivationCannotCreditRevokedSameOwnerContext_data()
    {
        QTest::addColumn<QString>("change"); for (const auto *change : {"token", "cache", "acl", "ready-token"}) QTest::newRow(change) << QString(change);
    }
    void directChatActivationCannotCreditRevokedSameOwnerContext()
    {
        QFETCH(QString, change); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        QTemporaryDir path; NativeCacheService replacement(path.path(), {}, true);
        bool revoked = false; QSignalSpy done(&controller, &NativeController::directChatOpened);
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (revoked || controller.selectedStream() != directId || controller.directChatBusy()
                || ((change == "ready-token") != (controller.directChatPhase() == "ready"))) return;
            revoked = true;
            if (change == "cache") controller.useCache(&replacement);
            else if (change == "acl") { auto stream = test.created; stream["role"] = "member"; QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream))); }
            else test.fixture.api.setAccessToken("rotated-at-activation");
        });
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(revoked);
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QVERIFY(controller.directChatUncertain()); QCOMPARE(test.posts, 1);
    }

    void directChatCompletedTargetEventsDoNotInvalidateAnUnrelatedNewForm_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"topic-delete", "stream-role", "binding-role"}) QTest::newRow(change) << QString(change);
    }
    void directChatCompletedTargetEventsDoNotInvalidateAnUnrelatedNewForm()
    {
        QFETCH(QString, change); DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
        const auto directKey = controller.directChatContextKey(), channelKey = controller.channelCreationContextKey();
        if (change == "topic-delete") QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", channelDefault}, {"stream_uuid", directId}})));
        else if (change == "stream-role") { auto row = test.created; row["role"] = "member"; QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row))); }
        else { auto row = creationBinding(userId, 900); row["stream_uuid"] = directId; row["role"] = "member";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream_binding.updated", row))); }
        QCOMPARE(controller.directChatContextKey(), directKey); QCOMPARE(controller.channelCreationContextKey(), channelKey);
        QCOMPARE(controller.directChatPhase(), "ready"); QVERIFY(controller.canStartDirectChatWith(channelMemberB));
        QVERIFY(controller.canCreateChannel()); QCOMPARE(test.posts, 1); QCOMPARE(done.size(), 1);
    }
};

QTEST_MAIN(ControllerDirectChatTest)
#include "native-controller-direct-chat-test.moc"
