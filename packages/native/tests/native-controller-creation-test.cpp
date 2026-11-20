#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"

class ControllerCreationTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void topicCreationPublicWitnessesBindOnlyTheVerifiedOwnersIntent()
    {
        TopicCreationFixture test;
        auto &controller = test.fixture.controller;
        QVERIFY(controller.property("topicCreationAttemptId").toString().isEmpty());
        QVERIFY(controller.property("topicCreationContextKey").toString().isEmpty());
        QVERIFY(test.fixture.start());
        const auto key = controller.property("topicCreationContextKey").toString();
        QCOMPARE(key.size(), 64);
        controller.createTopic(streamId, " ");
        QVERIFY(controller.property("topicCreationAttemptId").toString().isEmpty());
        QCOMPARE(test.posts, 0);
        test.postStatus = 503;
        controller.createTopic(streamId, "Exact topic identity");
        const auto attempt = controller.property("topicCreationAttemptId").toString();
        QVERIFY(isUuid(attempt));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        QTRY_VERIFY(controller.topicCreationUncertain());
        QCOMPARE(test.posts, 1); QCOMPARE(test.created.value("uuid").toString(), attempt);
        QCOMPARE(controller.property("topicCreationAttemptId").toString(), attempt);
        controller.checkTopicCreation();
        QTRY_VERIFY(!controller.topicCreationBusy());
        QVERIFY(!controller.topicCreationUncertain());
        QCOMPARE(controller.property("topicCreationAttemptId").toString(), attempt);
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 1); QCOMPARE(test.catalogs, 1);
        controller.logout();
        QVERIFY(controller.property("topicCreationAttemptId").toString().isEmpty());
        QVERIFY(controller.property("topicCreationContextKey").toString().isEmpty());
        QVERIFY(test.fixture.start());
        QCOMPARE(controller.property("topicCreationAttemptId").toString(), attempt);
    }

    void topicCreationExactSignalIdentitySurvivesAnEarlierListenerStartingTheNextIntent()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        bool startedNext = false, laterFormOpen = false;
        QString firstId, boundId;
        connect(&controller, &NativeController::topicCreated, &controller, [&](const QString &stream, const QString &id) {
            if (startedNext) return;
            startedNext = true; firstId = id; laterFormOpen = true;
            test.postDelay = 200;
            controller.createTopic(stream, "Same name");
            boundId = controller.property("topicCreationAttemptId").toString();
        });
        connect(&controller, &NativeController::topicCreated, &controller, [&](const QString &stream, const QString &id) {
            // The signal is already being delivered. Exact UUID matching is the
            // UI's only way to avoid closing the newly admitted same-target form.
            if (stream == streamId && id == boundId) laterFormOpen = false;
        });
        controller.createTopic(streamId, "Same name");
        QTRY_COMPARE(test.posts, 2);
        QVERIFY(startedNext); QVERIFY(isUuid(firstId)); QVERIFY(isUuid(boundId));
        QVERIFY(firstId != boundId); QVERIFY(laterFormOpen);
        QVERIFY(controller.topicCreationBusy()); QCOMPARE(controller.topicCreationName(), QString("Same name"));
    }

    void topicCreationContextIgnoresBenignRowsAndCompletedOldIntentEvents()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        QSignalSpy completed(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Completed topic"); QTRY_COMPARE(completed.size(), 1);
        const auto key = controller.property("topicCreationContextKey").toString();
        QCOMPARE(key.size(), 64);
        auto topic = test.created; topic["name"] = "Renamed completed topic";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", topic)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        auto stream = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Benign label"},
            {"encryption", false}, {"unread_count", 7}};
        const auto acl = controller.streams().first().toMap().value("contextKey").toString();
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream, 13)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", userId}, {"username", "new-label"}}, 14)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        stream["role"] = "member";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream, 15)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        QVERIFY(controller.streams().first().toMap().value("contextKey").toString() != acl);
        QCOMPARE(completed.size(), 1);
    }

    void topicCreationContextChangesOnlyOnTheActualShellLifetime_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"token", "route", "catalog", "cache", "logout"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void topicCreationContextChangesOnlyOnTheActualShellLifetime()
    {
        QFETCH(QString, change);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        TopicCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        const auto key = controller.property("topicCreationContextKey").toString();
        QCOMPARE(key.size(), 64);
        if (change == "token") test.fixture.api.setAccessToken("topic-lifetime-rotated-token");
        else if (change == "route") controller.selectTopic({});
        else if (change == "catalog") controller.refresh();
        else if (change == "cache") controller.useCache(&cache);
        else controller.logout();
        QVERIFY(controller.property("topicCreationContextKey").toString() != key);
        if (change == "logout") QVERIFY(controller.property("topicCreationContextKey").toString().isEmpty());
        QCOMPARE(test.posts, 0);
    }

    void topicPublicationSubscribersCanDestroyTheController_data()
    {
        QTest::addColumn<QString>("window");
        for (const auto *window : {"stream", "topic", "final"})
            QTest::newRow(window) << QString::fromLatin1(window);
    }
    void topicPublicationSubscribersCanDestroyTheController()
    {
        QFETCH(QString, window);
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> current = new NativeController(&test.fixture.api, &test.fixture.session);
        current->refresh(); QTRY_COMPARE(current->streams().size(), 1);
        current->selectStream(streamId); QTRY_COMPARE(current->topics().size(), 1);
        current->selectTopic(topicId); QTRY_VERIFY(!current->busy());
        connect(current, &NativeController::stateChanged, &test.fixture.session, [&] {
            if (!current || test.created.isEmpty()) return;
            const bool location = window == "stream" ? current->selectedTopic().isEmpty()
                : current->selectedTopic() == test.created.value("uuid").toString();
            const bool stage = window == "final" ? !current->topicCreationBusy() : current->topicCreationBusy();
            if (location && stage) delete current.data();
        });
        current->createTopic(streamId, "Destroyed publication subscriber");
        QTRY_VERIFY(current.isNull());
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 1); QCOMPARE(test.catalogs, 1);
    }

    void topicPublicationLifetimeFence_data()
    {
        QTest::addColumn<QString>("window");
        QTest::addColumn<QString>("change");
        for (const auto *window : {"stream", "topic", "final"})
            for (const auto *change : {"token", "cache", "acl"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(window, change)))
                    << QString::fromLatin1(window) << QString::fromLatin1(change);
    }

    void topicPublicationLifetimeFence()
    {
        QFETCH(QString, window); QFETCH(QString, change);
        QTemporaryDir first, second;
        NativeCacheService cache(first.path(), {}, true), replacement(second.path(), {}, true);
        TopicCreationFixture test;
        if (change == "cache") test.fixture.controller.useCache(&cache);
        QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        QSignalSpy completed(&controller, &NativeController::topicCreated);
        bool changed = false, accepted = true;
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (changed || test.created.isEmpty()) return;
            const auto id = test.created.value("uuid").toString();
            const bool location = window == "stream" ? controller.selectedStream() == streamId && controller.selectedTopic().isEmpty()
                : controller.selectedStream() == streamId && controller.selectedTopic() == id;
            const bool stage = window == "final" ? !controller.topicCreationBusy() : controller.topicCreationBusy();
            if (!location || !stage) return;
            changed = true;
            if (change == "token") test.fixture.api.setAccessToken("topic-publication-rotated-token");
            else if (change == "cache") controller.useCache(&replacement);
            else {
                auto row = creationChannel(streamId, "ACL changed during publication");
                row["role"] = "member";
                accepted = controller.applyRealtimeEvent(controllerEvent("stream.updated", row));
            }
        });
        controller.createTopic(streamId, "Publication fence");
        QTRY_VERIFY(changed); QVERIFY(accepted);
        QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(test.posts, 1);
        QCOMPARE(completed.size(), 0);
        QVERIFY(!controller.topicCreationBusy()); QVERIFY(controller.topicCreationUncertain());
        const auto attempt = controller.topicCreationAttemptId();
        QCOMPARE(attempt, test.created.value("uuid").toString());
        controller.refresh(); QTRY_VERIFY(controller.canCheckTopicCreation());
        controller.checkTopicCreation(); QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(controller.topicCreationAttemptId(), attempt); QCOMPARE(test.posts, 1);
    }

    void topicPublicationSameTargetReplacementDoesNotCloseLaterForm()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        bool replaced = false, laterFormOpen = false;
        QString firstId;
        QSignalSpy completed(&controller, &NativeController::topicCreated);
        connect(&controller, &NativeController::topicCreated, &controller, [&](const QString &stream, const QString &) {
            // This is the actual current Main.qml completion rule: stream only.
            if (stream == streamId) laterFormOpen = false;
        });
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (replaced || test.created.isEmpty() || controller.topicCreationBusy()
                || controller.selectedTopic() != test.created.value("uuid").toString()) return;
            replaced = true;
            firstId = test.created.value("uuid").toString();
            laterFormOpen = true;
            test.postDelay = 200;
            controller.createTopic(streamId, "Same name");
        });
        controller.createTopic(streamId, "Same name");
        QTRY_COMPARE(test.posts, 2);
        QVERIFY(replaced); QVERIFY(!firstId.isEmpty());
        QVERIFY(controller.topicCreationBusy());
        QCOMPARE(controller.topicCreationName(), QString("Same name"));
        QCOMPARE(controller.topicCreationStream(), streamId);
        QVERIFY(firstId != test.created.value("uuid").toString());
        QVERIFY(laterFormOpen);
        QCOMPARE(completed.size(), 0);
    }

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

    void encryptedChannelCreationSendsBootstrapKeyAndNavigates()
    {
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        QVERIFY(test.attachVault(keys));
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        const auto history = test.fixture.totalHistoryGets();
        // The reference clears the exclusive flags for an encrypted intent.
        controller.createChannel("  Protected channel  ", "  Frozen description  ", true, true, {channelMemberA}, true);
        QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY2(controller.channelCreationPhase() == "ready",
            qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(complete.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        const auto body = test.streamBodies.first();
        const auto keyUuid = body.value("current_encryption_key_uuid").toString();
        QVERIFY(isUuid(keyUuid));
        QCOMPARE(body, QJsonObject({{"uuid", controller.channelCreationForm().value("attemptId").toString()},
            {"name", "Protected channel"}, {"description", "Frozen description"}, {"source_name", "native"},
            {"invite_only", false}, {"announce", false}, {"encryption", true}, {"current_encryption_key_uuid", keyUuid}}));
        QCOMPARE(test.memberBodies.first(), QJsonObject({{"member", QJsonArray{channelMemberA}}}));
        // The generated key is committed to the durable vault bound to the created stream.
        const auto stream = controller.selectedStream();
        const auto stored = keys.read(keys.lease(), {stream, keyUuid});
        QCOMPARE(stored.failure, NativeChatKeyVault::Failure::None);
        QCOMPARE(stored.value.bytes.size(), 32);
        // Success navigation matches the ordinary path: verified aggregate + choose-topic prompt.
        QCOMPARE(controller.selectedStream(), test.created.value("uuid").toString());
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(controller.conversationKind(), QString("stream"));
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1);
    }

    void encryptedChannelCreationKeepsOneImmutableAttemptAcrossClicks()
    {
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        QVERIFY(test.attachVault(keys));
        test.postDelay = 200; QSignalSpy complete(&controller, &NativeController::channelCreated);
        controller.createChannel("Frozen intent", "", false, false, {}, true);
        QTRY_VERIFY(controller.channelCreationBusy());
        controller.createChannel("Ignored second click", "", true, true, {}, true);
        QVERIFY(controller.channelCreationBusy());
        QTRY_VERIFY(!controller.channelCreationBusy());
        // One POST for one intent; the frozen attempt uuid and key uuid are untouched.
        QCOMPARE(test.posts, 1);
        const auto attempt = controller.channelCreationForm().value("attemptId").toString();
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        QVERIFY(isUuid(attempt)); QVERIFY(isUuid(keyUuid));
        // An unresolved (uncertain) intent cannot fork a second attempt either.
        test.postDelay = 0; test.postStatus = 503; test.getStatus = 404;
        const auto retry = controller.channelCreationForm();
        QCOMPARE(retry.value("attemptId"), attempt);
    }

    void encryptedChannelCreationAmbiguousCheckCommitsKeyOnceWithoutSecondPost()
    {
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        QVERIFY(test.attachVault(keys));
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        test.postStatus = 503;
        controller.createChannel("Ambiguous encrypted", "", false, false, {}, true);
        QTRY_VERIFY(controller.channelCreationUncertain());
        QCOMPARE(test.posts, 1);
        const auto attempt = controller.channelCreationForm().value("attemptId").toString();
        test.postStatus = 201; controller.checkChannelCreation();
        QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY2(controller.channelCreationPhase() == "ready",
            qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(test.posts, 1);
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        const auto stored = keys.read(keys.lease(), {test.created.value("uuid").toString(), keyUuid});
        QCOMPARE(stored.failure, NativeChatKeyVault::Failure::None);
        QCOMPARE(complete.size(), 1);
        QCOMPARE(controller.channelCreationForm().value("attemptId"), attempt);
    }

    void encryptedChannelCreationEchoMismatchStaysUncertainAndRecoversByCheck()
    {
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        QVERIFY(test.attachVault(keys));
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        const auto before = controller.selectedStream();
        test.alterEncryptedPost = [](QJsonObject &row) { row["encryption"] = false; };
        controller.createChannel("Unconfirmed encryption", "", false, false, {}, true);
        QTRY_VERIFY(controller.channelCreationUncertain());
        QVERIFY(controller.channelCreationError().contains("did not confirm", Qt::CaseInsensitive));
        QCOMPARE(test.posts, 1); QCOMPARE(complete.size(), 0);
        QCOMPARE(controller.selectedStream(), before); // no navigation
        // The unconfirmed key was not committed; a GET-only check against the
        // authoritative echo completes the same intent and commits the key.
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        test.alterEncryptedPost = {};
        controller.checkChannelCreation();
        QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY2(controller.channelCreationPhase() == "ready",
            qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(test.posts, 1); QCOMPARE(complete.size(), 1);
        QCOMPARE(test.gets, 1);
        const auto stored = keys.read(keys.lease(), {test.created.value("uuid").toString(), keyUuid});
        QCOMPARE(stored.failure, NativeChatKeyVault::Failure::None);
    }

    void encryptedChannelCreationReplacedRuntimeCannotPublish()
    {
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        QVERIFY(test.attachVault(keys));
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        test.postDelay = 300;
        controller.createChannel("Replaced runtime", "", false, false, {}, true);
        QTRY_VERIFY(controller.channelCreationBusy());
        controller.logout();
        QTest::qWait(600); // let the in-flight POST response land on a replaced runtime
        QCOMPARE(complete.size(), 0);
        QVERIFY(controller.channelCreationForm().isEmpty());
        QCOMPARE(controller.channelCreationPhase(), QString("idle"));
        QVERIFY(controller.selectedStream().isEmpty());
        QVERIFY(!keys.available() || keys.listKeys(keys.lease(), test.created.value("uuid").toString()).values.isEmpty());
    }

    void encryptedChannelCreationWithoutCryptoStoreFailsLocally()
    {
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        controller.createChannel("No vault", "", false, false, {}, true);
        QVERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.posts, 0); QCOMPARE(complete.size(), 0);
        QCOMPARE(controller.channelCreationPhase(), QString("idle"));
        QVERIFY(!controller.channelCreationError().isEmpty());
        // The ordinary path is untouched by the missing vault.
        QSignalSpy ordinary(&controller, &NativeController::channelCreated);
        controller.createChannel("Ordinary still works", "", false, false, {}, false);
        QTRY_COMPARE(ordinary.size(), 1);
    }

    // Review findings 4574928575/4574928581 (critical): the stream POST has been
    // dispatched and the server has created the encrypted channel, but the
    // process ends before the canonical GET echo commits the key. A controller
    // restart that shares the same vault and secret store must recover the
    // frozen group key from durable state and commit it once the echo verifies.
    void encryptedChannelCreationSurvivesProcessLossAfterDispatch()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        auto release = std::make_shared<std::function<void()>>();
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, release](const Request &request) -> Response {
            auto response = inner(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/streams/")
                response.defer = [release](std::function<void()> send) { *release = std::move(send); };
            return response;
        };
        auto *first = new NativeController(&test.fixture.api, &test.fixture.session);
        QPointer<NativeController> firstGuard(first), restartGuard, secondGuard;
        const auto cleanup = qScopeGuard([&] {
            delete firstGuard.data(); delete restartGuard.data(); delete secondGuard.data();
        });
        first->useChatKeys(&keys);
        first->refresh(); QTRY_COMPARE(first->streams().size(), 1);
        first->createChannel("Crash window", "", false, false, {}, true);
        QTRY_COMPARE(test.posts, 1); // dispatched; the reply is still in flight
        delete first; // the process ends between dispatch and the canonical GET
        if (*release) (*release)(); *release = {}; // the stale reply lands with nobody listening
        auto *restarted = new NativeController(&test.fixture.api, &test.fixture.session);
        restartGuard = restarted;
        restarted->useChatKeys(&keys);
        const auto streamUuid = test.created.value("uuid").toString();
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        QVERIFY(isUuid(keyUuid));
        // The recovery fetches the stream by the staged identity, verifies the
        // encrypted echo and commits the key durably.
        QVERIFY2(QTest::qWaitFor([&] {
            auto stored = keys.read(keys.lease(), {streamUuid, keyUuid});
            const bool ok = stored.failure == NativeChatKeyVault::Failure::None && stored.value.bytes.size() == 32;
            stored.value.bytes.fill('\0');
            return ok;
        }, 5000), "recovered group key missing after restart");
        const auto getsAfterRecovery = test.gets;
        QVERIFY(getsAfterRecovery >= 1);
        delete restarted; restartGuard = nullptr;
        // The stage was cleared with the commit: another restart re-verifies nothing.
        auto *secondRestart = new NativeController(&test.fixture.api, &test.fixture.session);
        secondGuard = secondRestart;
        secondRestart->useChatKeys(&keys);
        QTest::qWait(400);
        QCOMPARE(test.gets, getsAfterRecovery);
        delete secondRestart; secondGuard = nullptr;
    }

    // Review finding 4574928581 (medium): the stage must exist durably while
    // the POST is in flight, must survive a restart whose canonical echo does
    // not verify (nothing may be committed from it), and must be cleared only
    // by a successful commit so the next restart can finish the recovery.
    void encryptedChannelCreationPendingStageSurvivesMismatchedEcho()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        auto release = std::make_shared<std::function<void()>>();
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, release](const Request &request) -> Response {
            auto response = inner(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/streams/")
                response.defer = [release](std::function<void()> send) { *release = std::move(send); };
            return response;
        };
        auto *first = new NativeController(&test.fixture.api, &test.fixture.session);
        QPointer<NativeController> firstGuard(first), restartGuard, finalGuard;
        const auto cleanup = qScopeGuard([&] {
            delete firstGuard.data(); delete restartGuard.data(); delete finalGuard.data();
        });
        first->useChatKeys(&keys);
        first->refresh(); QTRY_COMPARE(first->streams().size(), 1);
        first->createChannel("Mismatched echo", "", false, false, {}, true);
        QTRY_COMPARE(test.posts, 1);
        const auto attempt = test.streamBodies.first().value("uuid").toString();
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        const auto streamUuid = test.created.value("uuid").toString();
        // The stage is durable before the POST response ever arrives: the
        // crash window has a recoverable copy from the moment of dispatch.
        const auto staged = keys.readCreateStage(keys.lease(), attempt);
        QCOMPARE(staged.failure, NativeChatKeyVault::Failure::None);
        QVERIFY(staged.value.has_value());
        QCOMPARE(staged.value->key, keyUuid);
        QCOMPARE(staged.value->groupKey.size(), 32);
        delete first;
        if (*release) (*release)(); *release = {};
        // Restart with an echo that does not verify: recovery must neither
        // commit the key nor clear the durable stage.
        test.alterGet = [](QJsonObject &row) {
            row["current_encryption_key_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999999");
        };
        auto *restarted = new NativeController(&test.fixture.api, &test.fixture.session);
        restartGuard = restarted;
        restarted->useChatKeys(&keys);
        QTRY_COMPARE(test.gets, 1);
        QTest::qWait(300); // let any (wrong) commit or clear settle
        const auto uncommitted = keys.read(keys.lease(), {streamUuid, keyUuid});
        QVERIFY(uncommitted.value.bytes.isEmpty());
        const auto kept = keys.readCreateStage(keys.lease(), attempt);
        QCOMPARE(kept.failure, NativeChatKeyVault::Failure::None);
        delete restarted; restartGuard = nullptr;
        // A later restart against the authoritative echo completes the
        // recovery: the key commits and the stage is cleared with it.
        test.alterGet = {};
        auto *settled = new NativeController(&test.fixture.api, &test.fixture.session);
        finalGuard = settled;
        settled->useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] {
            auto stored = keys.read(keys.lease(), {streamUuid, keyUuid});
            const bool ok = stored.failure == NativeChatKeyVault::Failure::None && stored.value.bytes.size() == 32;
            stored.value.bytes.fill('\0');
            return ok;
        }, 5000), "stage recovery failed after the echo became authoritative");
        const auto cleared = keys.readCreateStage(keys.lease(), attempt);
        QCOMPARE(cleared.failure, NativeChatKeyVault::Failure::MissingKey);
        QVERIFY(keys.createStages(keys.lease()).values.isEmpty());
        delete settled; finalGuard = nullptr;
    }

    // Review finding (definite failure): a stream POST that is definitively
    // rejected did not create a stream, so the pre-dispatch stage is stale and
    // must not linger against the vault's create-stage budget.
    void encryptedChannelCreationDefiniteFailureClearsStage()
    {
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        QVERIFY(test.attachVault(keys));
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        test.postStatus = 400; // conclusive rejection: the server did not create the stream
        controller.createChannel("Rejected encrypted", "", false, false, {}, true);
        QTRY_VERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.posts, 1);
        QCOMPARE(complete.size(), 0);
        QVERIFY(!controller.channelCreationError().isEmpty());
        // The stage written before dispatch is closed with the rejection.
        QVERIFY2(keys.createStages(keys.lease()).values.isEmpty(),
            "definite POST failure left a stale create stage behind");
        // A later encrypted create is not starved by the rejected attempt.
        test.postStatus = 201;
        controller.createChannel("After rejection", "", false, false, {}, true);
        QVERIFY2(QTest::qWaitFor([&] { return controller.channelCreationPhase() == "ready"; }, 5000),
            qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        const auto keyUuid = test.streamBodies.last().value("current_encryption_key_uuid").toString();
        const auto stored = keys.read(keys.lease(), {test.created.value("uuid").toString(), keyUuid});
        QCOMPARE(stored.failure, NativeChatKeyVault::Failure::None);
        QCOMPARE(complete.size(), 1);
    }

    // Review finding (404 is not conclusive): a restart whose recovery GET
    // races the still-committing original POST must keep the durable stage;
    // deleting it on the first 404 can strand a channel that lands moments
    // later with its only key copy gone.
    void encryptedChannelCreationRecoveryKeepsStageWhileStreamMissing()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        auto release = std::make_shared<std::function<void()>>();
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, release](const Request &request) -> Response {
            auto response = inner(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/streams/")
                response.defer = [release](std::function<void()> send) { *release = std::move(send); };
            return response;
        };
        auto *first = new NativeController(&test.fixture.api, &test.fixture.session);
        QPointer<NativeController> firstGuard(first), restartGuard;
        const auto cleanup = qScopeGuard([&] { delete firstGuard.data(); delete restartGuard.data(); });
        first->useChatKeys(&keys);
        first->refresh(); QTRY_COMPARE(first->streams().size(), 1);
        first->createChannel("Racing recovery", "", false, false, {}, true);
        QTRY_COMPARE(test.posts, 1);
        const auto attempt = test.streamBodies.first().value("uuid").toString();
        delete first; // process loss; the POST reply never lands with a listener
        if (*release) (*release)(); *release = {};
        test.getStatus = 404; // the server has not finished committing the stream yet
        auto *restarted = new NativeController(&test.fixture.api, &test.fixture.session);
        restartGuard = restarted;
        restarted->useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return test.gets >= 2; }, 8000),
            "recovery did not re-check a missing stream within the session");
        const auto kept = keys.readCreateStage(keys.lease(), attempt);
        QCOMPARE(kept.failure, NativeChatKeyVault::Failure::None);
        QVERIFY(kept.value.has_value());
        const auto streamUuid = test.created.value("uuid").toString();
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        QVERIFY(keys.read(keys.lease(), {streamUuid, keyUuid}).value.bytes.isEmpty());
        // Once the stream lands, the in-session retry commits without a restart.
        test.getStatus = 200;
        QVERIFY2(QTest::qWaitFor([&] {
            auto stored = keys.read(keys.lease(), {streamUuid, keyUuid});
            const bool ok = stored.failure == NativeChatKeyVault::Failure::None && stored.value.bytes.size() == 32;
            stored.value.bytes.fill('\0');
            return ok;
        }, 8000), "stage not committed after the stream became visible to recovery");
        delete restarted; restartGuard = nullptr;
    }

    // Review finding (transient recovery failure): a failed scan must not burn
    // the per-session recovery for its vault lease; the scan retries within
    // the current session until the stage resolves.
    void encryptedChannelCreationRecoveryRetriesTransientFailureInSession()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        auto release = std::make_shared<std::function<void()>>();
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, release](const Request &request) -> Response {
            auto response = inner(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/streams/")
                response.defer = [release](std::function<void()> send) { *release = std::move(send); };
            return response;
        };
        auto *first = new NativeController(&test.fixture.api, &test.fixture.session);
        QPointer<NativeController> firstGuard(first), restartGuard;
        const auto cleanup = qScopeGuard([&] { delete firstGuard.data(); delete restartGuard.data(); });
        first->useChatKeys(&keys);
        first->refresh(); QTRY_COMPARE(first->streams().size(), 1);
        first->createChannel("Transient recovery", "", false, false, {}, true);
        QTRY_COMPARE(test.posts, 1);
        delete first;
        if (*release) (*release)(); *release = {};
        test.getStatus = 500; // the first recovery GET fails transiently
        auto *restarted = new NativeController(&test.fixture.api, &test.fixture.session);
        restartGuard = restarted;
        restarted->useChatKeys(&keys);
        QVERIFY(QTest::qWaitFor([&] { return test.gets >= 1; }, 5000));
        test.getStatus = 200; // the transient outage ends
        const auto streamUuid = test.created.value("uuid").toString();
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        QVERIFY2(QTest::qWaitFor([&] {
            auto stored = keys.read(keys.lease(), {streamUuid, keyUuid});
            const bool ok = stored.failure == NativeChatKeyVault::Failure::None && stored.value.bytes.size() == 32;
            stored.value.bytes.fill('\0');
            return ok;
        }, 8000), "recovery did not retry a transient failure within the same session");
        const auto attempt = test.streamBodies.first().value("uuid").toString();
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
        delete restarted; restartGuard = nullptr;
    }

    // Review finding 4575672241 (high): a crash between the durable stage
    // write and the stream POST dispatch leaves a stage whose POST provably
    // never left the process — nothing can ever land for it. Recovery must
    // reclaim those stages so a bounded run of such crashes cannot fill the
    // vault's create-stage budget and permanently starve encrypted creates.
    void encryptedChannelCreationCrashBeforeDispatchStagesCannotStarveLaterCreates()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        QSet<QString> attempts;
        int recoveryGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &attempts, &recoveryGets](const Request &request) -> Response {
            const auto prefix = QStringLiteral("/api/workspace/v1/messenger/streams/");
            const auto path = request.target.path();
            if (request.method == "GET" && path.startsWith(prefix) && path.size() > prefix.size()
                && attempts.contains(path.mid(prefix.size()))) {
                ++recoveryGets; // the never-created stream is absent: conclusive for a live POST, meaningless here
                return {QJsonObject{}, 404};
            }
            return inner(request);
        };
        // Simulate the maximum run of process losses inside the stage-to-
        // dispatch window: every victim left its durable stage behind.
        for (int i = 0; i < int(NativeChatKeyVault::MaximumCreateStages); ++i) {
            NativeChatKeyVault::CreateStage stage{QUuid::createUuid().toString(QUuid::WithoutBraces),
                QUuid::createUuid().toString(QUuid::WithoutBraces), QByteArray(32, char('a' + i)),
                1700000000000 + i};
            QCOMPARE(keys.putCreateStage(keys.lease(), stage), NativeChatKeyVault::Failure::None);
            attempts.insert(stage.attempt.toLower());
            stage.clearSecrets();
        }
        // A fresh authenticated controller scans the durable stages once.
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return keys.createStages(keys.lease()).values.isEmpty(); }, 8000),
            "never-dispatched create stages were never reclaimed; the create-stage budget stays exhausted");
        QCOMPARE(recoveryGets, 0); // reclaiming a provably never-dispatched stage needs no network round trip
        // The reclaimed budget proves the starvation is gone: an encrypted
        // create can stage its key and complete again.
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        controller.createChannel("After reclaim", "", false, false, {}, true);
        QVERIFY2(QTest::qWaitFor([&] { return controller.channelCreationPhase() == "ready"; }, 8000),
            qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(test.posts, 1);
        QCOMPARE(complete.size(), 1);
        QVERIFY(keys.createStages(keys.lease()).values.isEmpty());
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        auto stored = keys.read(keys.lease(), {test.created.value("uuid").toString(), keyUuid});
        QCOMPARE(stored.failure, NativeChatKeyVault::Failure::None);
        QCOMPARE(stored.value.bytes.size(), 32);
        stored.value.bytes.fill('\\0');
    }

    // Review finding 4575672242 (medium) guard: a stage whose POST was
    // already dispatched is NOT reclaimable — the request may still land
    // server-side — so a recovery 404 must retain it (the R2 contract). The
    // reclaim path may only ever retire stages that provably never left.
    void encryptedChannelCreationDispatchAttemptedStageSurvivesRecoveryScan()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        auto release = std::make_shared<std::function<void()>>();
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, release](const Request &request) -> Response {
            auto response = inner(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/streams/")
                response.defer = [release](std::function<void()> send) { *release = std::move(send); };
            return response;
        };
        auto *first = new NativeController(&test.fixture.api, &test.fixture.session);
        QPointer<NativeController> firstGuard(first), restartGuard;
        const auto cleanup = qScopeGuard([&] { delete firstGuard.data(); delete restartGuard.data(); });
        first->useChatKeys(&keys);
        first->refresh(); QTRY_COMPARE(first->streams().size(), 1);
        first->createChannel("Dispatched then crashed", "", false, false, {}, true);
        QTRY_COMPARE(test.posts, 1); // the POST left the process; the reply is still in flight
        const auto attempt = test.streamBodies.first().value("uuid").toString();
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        delete first; // process loss after dispatch; nobody consumes the reply
        if (*release) (*release)(); *release = {};
        test.getStatus = 404; // the stream is not visible yet when recovery checks
        auto *restarted = new NativeController(&test.fixture.api, &test.fixture.session);
        restartGuard = restarted;
        restarted->useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return test.gets >= 1; }, 5000), "recovery did not re-check the dispatched stream");
        QTest::qWait(300); // let any (wrong) retire or commit settle
        const auto kept = keys.readCreateStage(keys.lease(), attempt);
        QCOMPARE(kept.failure, NativeChatKeyVault::Failure::None);
        QVERIFY(kept.value.has_value());
        auto uncommitted = keys.read(keys.lease(), {test.created.value("uuid").toString(), keyUuid});
        QVERIFY(uncommitted.value.bytes.isEmpty());
        uncommitted.value.bytes.fill('\\0');
    }

    // Review finding 4576535469 (critical, R6): a dispatched POST whose
    // response was never observed (crash or timeout) may still commit
    // server-side long after ANY number of recovery 404s — the client can
    // observe no server-side deadline or cancellation for the attempt, so
    // absence establishes no definitive terminal outcome, and the stage may
    // hold the only copy of the group key. The R4 conclusive-absence reclaim
    // (5 definitive 404s across separate cycles + minimum age) therefore
    // must NOT delete dispatch-attempted stage material: the stage goes
    // DORMANT (durable absence counter saturated, key retained) and when the
    // stream finally lands, the recovery re-adopts it and commits the key.
    // Pre-fix this test fails: the aged stage is retired once five cycles
    // have answered 404, and the key is gone before the late commit lands.
    void encryptedChannelCreationDispatchAttemptedKeySurvivesConclusiveAbsence()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        const auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray groupKey(32, char('a'));
        // The crashed process left a dispatch-attempted stage whose POST was
        // handed to the network stack but whose response was never observed,
        // aged far past the old reclaim window: the exact scenario in which
        // the server may still commit the channel minutes to hours later.
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, groupKey,
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), attempt), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        auto visible = std::make_shared<std::atomic<bool>>(false);
        int recoveryGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, visible, &attempt, &keyUuid,
            &recoveryGets](const Request &request) -> Response {
            const auto target = QStringLiteral("/api/workspace/v1/messenger/streams/") + attempt;
            if (request.method == "GET" && request.target.path() == target) {
                ++recoveryGets;
                if (!visible->load()) return {QJsonObject{}, 404}; // the late commit has not landed yet
                auto row = creationChannel(attempt); // and now it lands
                row["encryption"] = true;
                row["current_encryption_key_uuid"] = keyUuid;
                return {row, 200};
            }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        // The recovery scans answer definitive 404s across cycle after cycle,
        // and the stage is far older than the old 10-minute reclaim floor.
        // The stage may saturate its absence evidence and go dormant, but it
        // must NEVER be retired: it is the only copy of the group key, and
        // absence alone proves nothing terminal. (Pre-fix: the reclaim fires
        // once five cycles and the age floor line up, deleting the key.)
        QVERIFY2(QTest::qWaitFor([&] {
            const auto stage = keys.readCreateStage(keys.lease(), attempt);
            return stage.failure != NativeChatKeyVault::Failure::None
                || stage.value->missingEvidence >= 5;
        }, 20000), "the recovery never recorded conclusive absence for the dispatched stage");
        auto kept = keys.readCreateStage(keys.lease(), attempt);
        QVERIFY2(kept.value.has_value(),
            "recovery destroyed the only group key on absence evidence alone, before any definitive terminal outcome");
        QCOMPARE(kept.value->groupKey.size(), 32);
        kept.value->clearSecrets();
        QVERIFY(keys.read(keys.lease(), {attempt, keyUuid}).value.bytes.isEmpty()); // not committed yet
        // The stream finally commits server-side. A later recovery scan must
        // re-adopt the dormant stage: verify the echo, commit the key, and
        // clear the stage — the channel becomes decryptable with the very
        // key the absence-based reclaim used to destroy.
        visible->store(true);
        QVERIFY2(QTest::qWaitFor([&] {
            auto stored = keys.read(keys.lease(), {attempt, keyUuid});
            const bool ok = stored.failure == NativeChatKeyVault::Failure::None && stored.value.bytes == groupKey;
            stored.value.bytes.fill('\0');
            return ok;
        }, 20000), "the dormant stage was not re-adopted after the late stream appeared");
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
    }

    // Review findings 4575672241/4575672242 residual hole, as re-scoped by
    // review finding 4576535469 (critical, R6): the R4 reclaim retired a
    // dispatch-attempted stage from absence evidence alone, which can delete
    // the only group key of a creation whose POST is still able to land.
    // Retiring dispatch-attempted stages on 404-count + age is therefore
    // REMOVED. Such stages now saturate their durable absence counter and go
    // DORMANT with their keys retained inside the vault's bounded 32-record
    // create-stage budget; recovery keeps re-checking them with the bounded
    // backoff re-arm. The budget is freed only by definitive terminal
    // outcomes: a verified encrypted echo (commit) or the live POST's own
    // definite rejection. This test asserts the new boundary AND the
    // residual starvation bound the design accepts: a full budget of dormant
    // stages rejects a new encrypted create locally rather than stealing an
    // unresolvable attempt's key — and once the dormant streams land, their
    // stages re-adopt, the budget frees, and creates complete again.
    void encryptedChannelCreationDispatchAttemptedStagesGoDormantWithoutKeyLoss()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        QHash<QString, QString> missingByKey; // attempt -> key uuid
        int recoveryGets = 0;
        auto visible = std::make_shared<std::atomic<bool>>(false);
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &missingByKey, &recoveryGets,
            visible](const Request &request) -> Response {
            const auto prefix = QStringLiteral("/api/workspace/v1/messenger/streams/");
            const auto path = request.target.path();
            if (request.method == "GET" && path.startsWith(prefix) && path.size() > prefix.size()
                && missingByKey.contains(path.mid(prefix.size()))) {
                ++recoveryGets;
                if (!visible->load()) return {QJsonObject{}, 404}; // missing for every cycle so far
                auto row = creationChannel(path.mid(prefix.size())); // the late commits land
                row["encryption"] = true;
                row["current_encryption_key_uuid"] = missingByKey.value(path.mid(prefix.size()));
                return {row, 200};
            }
            return inner(request);
        };
        // Simulate the maximum run of crashes inside the mark-to-socket
        // window: every victim is durably dispatch-attempted (its POST may
        // still land), and every stage has aged past the old reclaim window.
        for (int i = 0; i < int(NativeChatKeyVault::MaximumCreateStages); ++i) {
            NativeChatKeyVault::CreateStage stage{QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
                QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(), QByteArray(32, char('a' + i)),
                1700000000000 + i};
            QCOMPARE(keys.putCreateStage(keys.lease(), stage), NativeChatKeyVault::Failure::None);
            QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), stage.attempt), NativeChatKeyVault::Failure::None);
            missingByKey.insert(stage.attempt, stage.key);
            stage.clearSecrets();
        }
        // A fresh authenticated controller scans the durable stages; cycles
        // separated by the recovery retry intervals accumulate the absence
        // evidence on each stage. The stages go dormant — keys retained.
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] {
            const auto stages = keys.createStages(keys.lease());
            if (stages.values.size() != int(NativeChatKeyVault::MaximumCreateStages)) return false;
            for (const auto &stage : stages.values) {
                if (stage.missingEvidence < 5 || stage.groupKey.size() != 32) return false;
            }
            return true;
        }, 30000), "conclusively-missing dispatch-attempted stages were not all retained dormant with keys intact");
        // Evidence saturates at the dormancy threshold instead of growing,
        // and recovery stays armed for dormant stages: every stage has been
        // re-checked across separate scan cycles and never deleted.
        for (const auto &stage : keys.createStages(keys.lease()).values) QCOMPARE(stage.missingEvidence, 5);
        QVERIFY(recoveryGets >= 2 * int(NativeChatKeyVault::MaximumCreateStages));
        // Residual bound the design accepts (documented on-thread): the
        // dormant stages still hold the 32-record budget, so a new encrypted
        // create is rejected LOCALLY — no POST, no key destroyed — instead
        // of evicting an unresolvable attempt's only key copy.
        controller.createChannel("Starved while dormant", "", false, false, {}, true);
        QVERIFY2(QTest::qWaitFor([&] { return !controller.channelCreationBusy(); }, 8000),
            "a budget-starved encrypted create did not settle locally");
        QCOMPARE(test.posts, 0);
        QVERIFY(!controller.channelCreationError().isEmpty());
        QCOMPARE(keys.createStages(keys.lease()).values.size(), int(NativeChatKeyVault::MaximumCreateStages));
        // The dormant streams finally land: recovery re-adopts every stage,
        // commits its key, and frees the budget — a later create completes.
        visible->store(true);
        QVERIFY2(QTest::qWaitFor([&] { return keys.createStages(keys.lease()).values.isEmpty(); }, 45000),
            "dormant stages were not re-adopted after their late streams appeared");
        for (auto it = missingByKey.constBegin(); it != missingByKey.constEnd(); ++it) {
            auto stored = keys.read(keys.lease(), {it.key(), it.value()});
            QCOMPARE(stored.failure, NativeChatKeyVault::Failure::None);
            QCOMPARE(stored.value.bytes.size(), 32);
            stored.value.bytes.fill('\0');
        }
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        controller.createChannel("After dormant re-adoption", "", false, false, {}, true);
        QVERIFY2(QTest::qWaitFor([&] { return controller.channelCreationPhase() == "ready"; }, 8000),
            qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(test.posts, 1);
        QCOMPARE(complete.size(), 1);
        QVERIFY(keys.createStages(keys.lease()).values.isEmpty());
    }

    // Review finding 4575117269: a vault/server failure that lasts longer
    // than the bounded fast rescan window must not make recovery for the
    // lease go permanently silent until the next restart. After the fast
    // window is exhausted recovery stays armed with a bounded backoff re-arm
    // so a later-recovered server still commits pending stages in-session.
    void encryptedChannelCreationRecoveryReArmsAfterFastRetryWindowEnds()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        auto release = std::make_shared<std::function<void()>>();
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, release](const Request &request) -> Response {
            auto response = inner(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/streams/")
                response.defer = [release](std::function<void()> send) { *release = std::move(send); };
            return response;
        };
        auto *first = new NativeController(&test.fixture.api, &test.fixture.session);
        QPointer<NativeController> firstGuard(first), restartGuard;
        const auto cleanup = qScopeGuard([&] { delete firstGuard.data(); delete restartGuard.data(); });
        first->useChatKeys(&keys);
        first->refresh(); QTRY_COMPARE(first->streams().size(), 1);
        first->createChannel("Outage past the window", "", false, false, {}, true);
        QTRY_COMPARE(test.posts, 1);
        delete first;
        if (*release) (*release)(); *release = {};
        test.getStatus = 500; // the outage outlives the whole fast retry window
        auto *restarted = new NativeController(&test.fixture.api, &test.fixture.session);
        restartGuard = restarted;
        restarted->useChatKeys(&keys);
        // Initial scan plus the 5 bounded fast retries: after this the fast
        // window is exhausted while the failure is still in effect.
        QVERIFY2(QTest::qWaitFor([&] { return test.gets >= 6; }, 20000),
            "the bounded fast rescan window did not run its course");
        test.getStatus = 200; // the outage ends only after the window expired
        const auto streamUuid = test.created.value("uuid").toString();
        const auto keyUuid = test.streamBodies.first().value("current_encryption_key_uuid").toString();
        QVERIFY2(QTest::qWaitFor([&] {
            auto stored = keys.read(keys.lease(), {streamUuid, keyUuid});
            const bool ok = stored.failure == NativeChatKeyVault::Failure::None && stored.value.bytes.size() == 32;
            stored.value.bytes.fill('\\0');
            return ok;
        }, 20000), "recovery stayed permanently silent after the fast retry window expired");
        const auto attempt = test.streamBodies.first().value("uuid").toString();
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
        delete restarted; restartGuard = nullptr;
    }

    // Review finding 4576823403 (medium, R7): the dormant quarantine (R6)
    // keeps a conclusively-missing attempt's key forever — correct for the
    // key, but a full 32-record create budget of dormant attempts then
    // rejects every later encrypted create across restarts with NO way out,
    // because automatic recovery may never infer a terminal outcome from
    // absence alone. The budget must therefore never be PERMANENTLY starved:
    // the controller exposes the unresolved-attempt inventory and an
    // explicit user-invokable resolve. The human is the terminal authority:
    // resolving an attempt whose stream still answers a definitive 404
    // releases the stage record (an explicit discard the human ordered,
    // never inferred from absence) and frees the slot, so the 33rd
    // encrypted create succeeds. Resolving an attempt whose stream NOW
    // exists must re-adopt (commit) the key instead of discarding it.
    void encryptedChannelCreationDormantBudgetIsUserResolvable()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        QVector<QString> attempts; QHash<QString, QString> missingByKey; // attempt -> key uuid
        // The first seeded attempt is the one stream that lands late (only
        // while the user is resolving it, so the resolve must re-adopt).
        const auto late = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        auto lateVisible = std::make_shared<std::atomic<bool>>(false);
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &missingByKey, &late,
            lateVisible](const Request &request) -> Response {
            const auto prefix = QStringLiteral("/api/workspace/v1/messenger/streams/");
            const auto path = request.target.path();
            const auto id = path.startsWith(prefix) ? path.mid(prefix.size()) : QString();
            if (request.method == "GET" && missingByKey.contains(id)) {
                if (id != late || !lateVisible->load()) return {QJsonObject{}, 404};
                auto row = creationChannel(late); // the late commit landed
                row["encryption"] = true;
                row["current_encryption_key_uuid"] = missingByKey.value(late);
                return {row, 200};
            }
            return inner(request);
        };
        for (int i = 0; i < int(NativeChatKeyVault::MaximumCreateStages); ++i) {
            NativeChatKeyVault::CreateStage stage{i == 0 ? late
                    : QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
                QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(), QByteArray(32, char('a' + i)),
                1700000000000 + i};
            QCOMPARE(keys.putCreateStage(keys.lease(), stage), NativeChatKeyVault::Failure::None);
            QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), stage.attempt), NativeChatKeyVault::Failure::None);
            missingByKey.insert(stage.attempt, stage.key);
            attempts.append(stage.attempt);
            stage.clearSecrets();
        }
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        // Reproduce the accepted R6 residual bound first: every stage
        // saturates its absence evidence and the whole budget is held.
        QVERIFY2(QTest::qWaitFor([&] {
            const auto stages = keys.createStages(keys.lease());
            if (stages.values.size() != int(NativeChatKeyVault::MaximumCreateStages)) return false;
            for (const auto &stage : stages.values)
                if (stage.missingEvidence < 5 || stage.groupKey.size() != 32) return false;
            return true;
        }, 30000), "the dormant-full-budget scenario did not reproduce");
        // The inventory: every unresolved attempt is visible with its
        // identity, created timestamp and dormant flag — never its key.
        const auto inventory = controller.unresolvedChannelCreateStages();
        QCOMPARE(inventory.size(), int(NativeChatKeyVault::MaximumCreateStages));
        for (const auto &value : inventory) {
            const auto row = value.toObject();
            const auto attempt = row.value("attempt").toString();
            QVERIFY2(missingByKey.contains(attempt), qPrintable(attempt));
            QCOMPARE(row.value("keyUuid").toString(), missingByKey.value(attempt));
            QVERIFY(row.value("dispatchAttempted").toBool());
            QVERIFY(row.value("dormant").toBool());
            QVERIFY(row.value("createdAt").toDouble() >= 1700000000000.0);
            QVERIFY(!row.contains("groupKey")); // key material never leaves the vault
        }
        // Resolving an attempt that is not staged changes nothing.
        {
            QSignalSpy bogus(&controller, &NativeController::channelCreateStageResolved);
            controller.resolveChannelCreateStage(QUuid::createUuid().toString(QUuid::WithoutBraces));
            QVERIFY2(QTest::qWaitFor([&] { return bogus.size() == 1; }, 5000),
                "resolving an unknown attempt never settled");
            QCOMPARE(bogus.at(0).at(1).toString(), QStringLiteral("unresolved"));
            QCOMPARE(keys.createStages(keys.lease()).values.size(), int(NativeChatKeyVault::MaximumCreateStages));
        }
        // The 33rd encrypted create is rejected locally while the budget is
        // fully held by dormant attempts.
        controller.createChannel("Starved while dormant", "", false, false, {}, true);
        QVERIFY2(QTest::qWaitFor([&] { return !controller.channelCreationBusy(); }, 8000),
            "a budget-starved encrypted create did not settle locally");
        QCOMPARE(test.posts, 0);
        QVERIFY(!controller.channelCreationError().isEmpty());
        // Review finding 4576938727 (critical, R8): the user checks one
        // attempt whose stream is still absent. A 404 at resolve time is NOT
        // a terminal outcome — a delayed POST can still land and commit that
        // channel — so the check must KEEP the stage and its key and simply
        // report "not found yet". (Pre-fix RED: resolve destroyed the stage
        // on a bare 404, deleting the only copy of a possibly-live key.)
        const auto checked = attempts.last();
        {
            QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
            controller.resolveChannelCreateStage(checked);
            QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
                "checking a missing attempt never settled");
            QCOMPARE(done.at(0).at(0).toString(), checked);
            QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("unresolved"));
        }
        QVERIFY(keys.readCreateStage(keys.lease(), checked).value.has_value());
        QCOMPARE(controller.unresolvedChannelCreateStages().size(), int(NativeChatKeyVault::MaximumCreateStages));
        // The budget is still full: keeping the checked attempt keeps its
        // slot, so an encrypted create is still rejected until an explicit,
        // informed DISCARD closes the attempt the human refuses to await.
        controller.createChannel("Still starved", "", false, false, {}, true);
        QVERIFY2(QTest::qWaitFor([&] { return !controller.channelCreationBusy(); }, 8000),
            "a budget-starved encrypted create did not settle locally");
        QCOMPARE(test.posts, 0);
        QVERIFY(!controller.channelCreationError().isEmpty());
        // An unconfirmed discard is refused and changes nothing — the key
        // removal must ride on an explicit confirmation the UI names.
        {
            QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
            controller.discardChannelCreateStage(checked, false);
            QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
                "an unconfirmed discard never settled");
            QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("unresolved"));
        }
        QVERIFY(keys.readCreateStage(keys.lease(), checked).value.has_value());
        // The informed explicit discard: frees the budget slot and removes
        // the staged key record exactly as the warned user asked.
        {
            QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
            controller.discardChannelCreateStage(checked, true);
            QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
                "the confirmed discard never settled");
            QCOMPARE(done.at(0).at(0).toString(), checked);
            QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("discarded"));
        }
        QCOMPARE(keys.readCreateStage(keys.lease(), checked).failure, NativeChatKeyVault::Failure::MissingKey);
        QCOMPARE(controller.unresolvedChannelCreateStages().size(), int(NativeChatKeyVault::MaximumCreateStages) - 1);
        // The freed slot: the 33rd encrypted create now goes through and
        // completes end-to-end.
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        controller.createChannel("After user resolve", "", false, false, {}, true);
        QVERIFY2(QTest::qWaitFor([&] { return controller.channelCreationPhase() == "ready"; }, 8000),
            qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(test.posts, 1);
        QCOMPARE(complete.size(), 1);
        // Resolve is never a silent key discard: when the attempt's stream
        // NOW exists, resolving re-adopts (commits) the key instead.
        *lateVisible = true;
        QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
        controller.resolveChannelCreateStage(late);
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 8000),
            "resolving the landed attempt never settled");
        QCOMPARE(done.at(0).at(0).toString(), late);
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("committed"));
        auto stored = keys.read(keys.lease(), {late, missingByKey.value(late)});
        QCOMPARE(stored.failure, NativeChatKeyVault::Failure::None);
        QCOMPARE(stored.value.bytes, QByteArray(32, char('a'))); // committed, not discarded
        stored.value.bytes.fill('\\0');
        QCOMPARE(keys.readCreateStage(keys.lease(), late).failure, NativeChatKeyVault::Failure::MissingKey);
    }

    // Review finding 4576938727 (critical, R8): the client can never prove a
    // terminal outcome from absence — a dispatch-attempted create POST may
    // still land arbitrarily late, and its stage may hold the only copy of
    // the group key. A user-invokable check must therefore treat a 404 as
    // "not found yet": report unresolved and retain the stage and key
    // byte-for-byte. Only the positive proof (the stream served with a
    // verifying encrypted echo) is terminal for a check, and it commits the
    // key instead of discarding it. (Pre-fix RED: resolve cleared the stage
    // on a bare 404 and reported "released".)
    void encryptedChannelCreationUserResolveKeepsKeyOnMissingStream()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        const auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray groupKey(32, char('c'));
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, groupKey,
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), attempt), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        auto visible = std::make_shared<std::atomic<bool>>(false);
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, visible, &attempt,
            &keyUuid](const Request &request) -> Response {
            const auto target = QStringLiteral("/api/workspace/v1/messenger/streams/") + attempt;
            if (request.method == "GET" && request.target.path() == target) {
                if (!visible->load()) return {QJsonObject{}, 404}; // the late commit has not landed
                auto row = creationChannel(attempt); // and now it lands
                row["encryption"] = true;
                row["current_encryption_key_uuid"] = keyUuid;
                return {row, 200};
            }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        {
            QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
            controller.resolveChannelCreateStage(attempt);
            QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
                "the user check never settled");
            QCOMPARE(done.at(0).at(0).toString(), attempt);
            QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("unresolved"));
        }
        auto kept = keys.readCreateStage(keys.lease(), attempt);
        QVERIFY2(kept.value.has_value(),
            "a resolve 404 destroyed the only staged group key although a delayed POST could still land");
        QCOMPARE(kept.value->groupKey.size(), 32);
        kept.value->clearSecrets();
        QVERIFY(keys.read(keys.lease(), {attempt, keyUuid}).value.bytes.isEmpty()); // not committed
        // The channel lands after all. The check's positive-proof branch
        // commits the key and frees the slot (re-adoption, never discard).
        *visible = true;
        QSignalSpy landed(&controller, &NativeController::channelCreateStageResolved);
        controller.resolveChannelCreateStage(attempt);
        QVERIFY2(QTest::qWaitFor([&] { return landed.size() == 1; }, 8000),
            "checking the landed attempt never settled");
        QCOMPARE(landed.at(0).at(1).toString(), QStringLiteral("committed"));
        auto stored = keys.read(keys.lease(), {attempt, keyUuid});
        QCOMPARE(stored.value.bytes, groupKey);
        stored.value.bytes.fill('\0');
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
    }

    // Issue #293: a user-invokable check holds its attempt in the transient
    // resolving set before dispatching the canonical GET. A normal same-owner
    // token renewal replaces the access token, which bumps the API generation
    // and aborts the in-flight GET; the shared request wrapper then drops the
    // response callback, so nothing ever settled the marker. Pre-fix: the check
    // stays "running" forever — a later Check silently returns and the only
    // way out was an application restart. The renewal must instead settle the
    // interrupted check as unresolved (durable stage and key untouched), the
    // retry must dispatch again, and a stale completion from the retired
    // generation must not disturb the newer check (operation tickets).
    void encryptedChannelCreateStageCheckRenewalInterruptRetryAndStaleCompletion()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        const auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray groupKey(32, char('r'));
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, groupKey,
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), attempt), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        auto visible = std::make_shared<std::atomic<bool>>(false);
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        int stageGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &attempt, &keyUuid, &stageGets,
            visible, hold, releases](const Request &request) -> Response {
            const auto target = QStringLiteral("/api/workspace/v1/messenger/streams/") + attempt;
            if (request.method == "GET" && request.target.path() == target) {
                ++stageGets;
                Response response{QJsonObject{}, 404}; // not found yet
                if (visible->load()) {
                    auto row = creationChannel(attempt); // the late commit landed
                    row["encryption"] = true;
                    row["current_encryption_key_uuid"] = keyUuid;
                    response = Response{row, 200};
                }
                if (hold->load()) response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
                return response;
            }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= 1; }, 5000),
            "the automatic recovery pass never scanned the seeded stage");
        QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
        controller.resolveChannelCreateStage(attempt);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= 2; }, 5000),
            "the user check never dispatched its GET");
        QCOMPARE(done.size(), 0); // the deferred GET keeps the check running
        // A normal same-owner renewal: the access token is replaced (the
        // account and vault lease are untouched), retiring every request
        // dispatched under the old generation.
        test.fixture.api.setAccessToken("synthetic-renewed-access-token");
        // AC1: the interrupted check settles as unresolved, naming the
        // interruption, and the durable stage/key survive byte-for-byte.
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
            "a check interrupted by token renewal never settled");
        QCOMPARE(done.at(0).at(0).toString(), attempt);
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("unresolved"));
        QVERIFY2(done.at(0).at(2).toString().contains("interrupt", Qt::CaseInsensitive),
            qPrintable(done.at(0).at(2).toString()));
        auto kept = keys.readCreateStage(keys.lease(), attempt);
        QVERIFY(kept.value.has_value());
        QCOMPARE(kept.value->groupKey.size(), 32);
        kept.value->clearSecrets();
        // AC4 (operation identity): keep the stale sockets undelivered, retry
        // the check under the new generation, then deliver only the retired
        // completions while the newer check is still running.
        const auto staleSends = releases->size(); // responses captured pre-renewal
        const auto getsBeforeRetry = stageGets;
        controller.resolveChannelCreateStage(attempt);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= getsBeforeRetry + 1; }, 5000),
            "the check could not be retried after the token renewal");
        for (size_t i = 0; i < staleSends; ++i) (*releases)[i](); // retired-generation answers
        QTest::qWait(300); // no retired callback may settle or re-open anything
        QCOMPARE(done.size(), 1); // only the interrupted settle so far
        const auto getsWhileNewerRuns = stageGets;
        controller.resolveChannelCreateStage(attempt); // still running: silently ignored
        QTest::qWait(200);
        QCOMPARE(done.size(), 1);
        QCOMPARE(stageGets, getsWhileNewerRuns); // the newer check's marker survived
        // The retry's own (current-generation) answer then settles it.
        hold->store(false);
        for (size_t i = staleSends; i < releases->size(); ++i) (*releases)[i]();
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 2; }, 5000),
            "the retried check never settled");
        QCOMPARE(done.at(1).at(0).toString(), attempt);
        QCOMPARE(done.at(1).at(1).toString(), QStringLiteral("unresolved"));
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::None);
    }

    // Issue #293: the confirmed informed discard refused with "a check is
    // already running" while the cancelled check's transient marker lingered
    // after a token renewal — even though no request remained in flight. The
    // renewal retires the running marker, so the user's explicit discard
    // proceeds; the durable key stays retained until that discard acts.
    void encryptedChannelCreateStageDiscardNotBlockedByCancelledCheck()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        const auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray groupKey(32, char('s'));
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, groupKey,
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), attempt), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        int stageGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &attempt, &stageGets,
            hold, releases](const Request &request) -> Response {
            const auto target = QStringLiteral("/api/workspace/v1/messenger/streams/") + attempt;
            if (request.method == "GET" && request.target.path() == target) {
                ++stageGets;
                Response response{QJsonObject{}, 404};
                if (hold->load()) response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
                return response;
            }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= 1; }, 5000),
            "the automatic recovery pass never scanned the seeded stage");
        QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
        controller.resolveChannelCreateStage(attempt);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= 2; }, 5000),
            "the user check never dispatched its GET");
        test.fixture.api.setAccessToken("synthetic-renewed-access-token");
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
            "a check interrupted by token renewal never settled");
        // Durable retention: cancellation touched nothing in the vault.
        auto kept = keys.readCreateStage(keys.lease(), attempt);
        QVERIFY(kept.value.has_value());
        QCOMPARE(kept.value->groupKey, groupKey);
        kept.value->clearSecrets();
        // AC2: the explicit informed discard is NOT blocked by the cancelled
        // check; it removes the record and frees the slot as ordered.
        controller.discardChannelCreateStage(attempt, true);
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 2; }, 5000),
            "the confirmed discard after a cancelled check never settled");
        QCOMPARE(done.at(1).at(0).toString(), attempt);
        QCOMPARE(done.at(1).at(1).toString(), QStringLiteral("discarded"));
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
        hold->store(false);
        for (auto &send : *releases) send();
    }

    // Issue #298: the automatic recovery scan keeps a stage snapshot while its
    // canonical GET is in flight; the informed discard is the terminal
    // authority and completes synchronously underneath that GET. A definitive
    // 404 answered after the discard must never run the missing-evidence
    // update against a record that no longer exists: the late bookkeeping is a
    // pure no-op (no crash, no resurrection, no mis-accounting), the scan
    // machinery keeps working for surviving stages, and the create budget
    // stays intact.
    void encryptedChannelCreateStageDiscardDuringRecoveryScanMakesLateFourOhFourNoOp()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        auto nextAttempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        // The recovery scan lists stages in vault address order, and the
        // create-stage address sorts by the attempt uuid string: keep the
        // surviving stage the smaller one so the scan provably processes the
        // survivor's 404 first - the worst case for asserting against the
        // discarded stage's still-pending answer.
        if (attempt < nextAttempt) std::swap(attempt, nextAttempt);
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, QByteArray(32, char('u')),
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        seed.dispatchAttempted = true; // its POST may still land: the scan GETs it
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        // A second dispatch-attempted stage survives: its own 404 must still
        // advance evidence normally, proving the scan and the bookkeeping are
        // unaffected around the fenced one.
        const auto nextKey = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        NativeChatKeyVault::CreateStage next{nextAttempt, nextKey, QByteArray(32, char('v')),
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        next.dispatchAttempted = true;
        QCOMPARE(keys.putCreateStage(keys.lease(), next), NativeChatKeyVault::Failure::None);
        next.clearSecrets();
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        int scanGets = 0;
        int survivorGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &attempt, &nextAttempt,
            &scanGets, &survivorGets, hold, releases](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto base = QStringLiteral("/api/workspace/v1/messenger/streams/");
            if (request.method == "GET" && (path == base + attempt || path == base + nextAttempt)) {
                ++scanGets;
                if (path == base + nextAttempt) ++survivorGets;
                Response response{QJsonObject{}, 404}; // definitive absence
                if (hold->load()) response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
                return response;
            }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return releases->size() >= 1; }, 5000),
            "the automatic recovery pass never scanned the seeded stages");
        // The scan chains its GETs one at a time and, with the forced uuid
        // order above, lists the surviving stage first. Let its 404 through so
        // the pass advances to the to-be-discarded stage: the informed discard
        // must run while that stage's OWN GET is in flight - the race this
        // case pins - and not earlier.
        (*releases)[0]();
        QVERIFY2(QTest::qWaitFor([&] { return scanGets >= 2 && releases->size() >= 2; }, 5000),
            "the recovery pass never reached the to-be-discarded stage's GET");
        // The discard settles immediately: the scan holds no user-facing
        // resolve marker that could block it.
        QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
        controller.discardChannelCreateStage(attempt, true);
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
            "the confirmed discard during the in-flight recovery scan never settled");
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("discarded"));
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
        // Release the discarded stage's late 404: its evidence update must be
        // a pure no-op against the record that no longer exists.
        (*releases)[1]();
        // Deterministic proof that the late callback actually ran before the
        // assertions below: its callback completes the pass with the surviving
        // stage unresolved, so the bounded 1 s recovery retry re-arms a fresh
        // pass whose re-GET of the surviving stage can only be observed after
        // EVERY first-pass callback - the discarded stage's included - has
        // been processed. The re-GET is the marker; nothing sleeps.
        QVERIFY2(QTest::qWaitFor([&] { return survivorGets >= 2; }, 5000),
            "the recovery pass never completed after the discarded stage's late answer");
        // The surviving stage's absence evidence still advanced exactly as
        // before.
        QVERIFY2(QTest::qWaitFor([&] {
            const auto stored = keys.readCreateStage(keys.lease(), nextAttempt);
            return stored.failure == NativeChatKeyVault::Failure::None && stored.value->missingEvidence >= 1;
        }, 5000), "the surviving stage's missing evidence never advanced");
        // The discarded stage stays gone: no resurrection, no torn record, and
        // the list stays clean and decodable.
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
        QVERIFY(keys.read(keys.lease(), {attempt, keyUuid}).value.bytes.isEmpty()); // key never committed
        auto listed = keys.createStages(keys.lease());
        QCOMPARE(listed.failure, NativeChatKeyVault::Failure::None);
        QCOMPARE(listed.values.size(), 1);
        QCOMPARE(listed.values.first().attempt, nextAttempt);
        // Budget accounting intact: the discarded slot is free for new work.
        NativeChatKeyVault::CreateStage fresh{QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
            QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(), QByteArray(32, char('w')),
            QDateTime::currentMSecsSinceEpoch()};
        QCOMPARE(keys.putCreateStage(keys.lease(), fresh), NativeChatKeyVault::Failure::None);
        fresh.clearSecrets();
    }

    // Issue #293: the automatic recovery pass excluded the API generation
    // from its once-per-owner/vault token, so after a token renewal retired
    // the in-flight stage GET, neither the dropped callback nor the unchanged
    // token ever re-armed the pass — recovery went silent for the rest of the
    // vault session and a channel that appeared later was never adopted. The
    // abandoned pass must resume once the same owner is ready again.
    void encryptedChannelCreateStageRecoveryResumesAfterTokenRenewal()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        const auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray groupKey(32, char('t'));
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, groupKey,
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), attempt), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        auto visible = std::make_shared<std::atomic<bool>>(false);
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        int stageGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &attempt, &keyUuid, &stageGets,
            visible, hold, releases](const Request &request) -> Response {
            const auto target = QStringLiteral("/api/workspace/v1/messenger/streams/") + attempt;
            if (request.method == "GET" && request.target.path() == target) {
                ++stageGets;
                Response response{QJsonObject{}, 404};
                if (visible->load()) {
                    auto row = creationChannel(attempt); // the late commit landed
                    row["encryption"] = true;
                    row["current_encryption_key_uuid"] = keyUuid;
                    response = Response{row, 200};
                }
                if (hold->load()) response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
                return response;
            }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= 1; }, 5000),
            "the automatic recovery pass never scanned the seeded stage");
        // The renewal interrupts the in-flight recovery GET. Pre-fix no
        // further scan was ever armed for this owner/vault lease.
        test.fixture.api.setAccessToken("synthetic-renewed-access-token");
        hold->store(false);
        for (auto &send : *releases) send(); // the interrupted response lands on a retired generation
        // AC3: without any restart, recovery resumes and adopts the channel
        // that only now appears — the key commits and the stage clears.
        visible->store(true);
        QVERIFY2(QTest::qWaitFor([&] {
            auto stored = keys.read(keys.lease(), {attempt, keyUuid});
            const bool ok = stored.failure == NativeChatKeyVault::Failure::None && stored.value.bytes == groupKey;
            stored.value.bytes.fill('\0');
            return ok && keys.readCreateStage(keys.lease(), attempt).failure != NativeChatKeyVault::Failure::None;
        }, 20000), "recovery interrupted by token renewal never resumed to adopt the verified channel");
    }

    // Issue #293: the same stranding across sign-out/re-login: the check
    // interrupted by logout must settle (clearSession bumps the generation),
    // and after the same owner signs in again the check must dispatch and
    // settle normally again instead of returning as "already running".
    void encryptedChannelCreateStageCheckSurvivesSignOutAndReLogin()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        const auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray groupKey(32, char('u'));
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, groupKey,
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), attempt), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        auto visible = std::make_shared<std::atomic<bool>>(false);
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        int stageGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &attempt, &keyUuid, &stageGets,
            visible, hold, releases](const Request &request) -> Response {
            const auto target = QStringLiteral("/api/workspace/v1/messenger/streams/") + attempt;
            if (request.method == "GET" && request.target.path() == target) {
                ++stageGets;
                Response response{QJsonObject{}, 404};
                if (visible->load()) {
                    auto row = creationChannel(attempt);
                    row["encryption"] = true;
                    row["current_encryption_key_uuid"] = keyUuid;
                    response = Response{row, 200};
                }
                if (hold->load()) response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
                return response;
            }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= 1; }, 5000),
            "the automatic recovery pass never scanned the seeded stage");
        QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
        controller.resolveChannelCreateStage(attempt);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets >= 2; }, 5000),
            "the user check never dispatched its GET");
        controller.logout(); // clearSession bumps the API generation too
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
            "a check interrupted by sign-out never settled");
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("unresolved"));
        auto kept = keys.readCreateStage(keys.lease(), attempt);
        QVERIFY(kept.value.has_value());
        kept.value->clearSecrets();
        // Sign back in as the same owner; the check must work again.
        QVERIFY2(test.fixture.start(), "the synthetic re-login failed");
        hold->store(false);
        for (auto &send : *releases) send();
        const auto getsBeforeRetry = stageGets;
        controller.resolveChannelCreateStage(attempt);
        QVERIFY2(QTest::qWaitFor([&] { return stageGets > getsBeforeRetry; }, 5000),
            "the check was still considered running after sign-out/re-login");
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 2; }, 5000),
            "the check after re-login never settled");
        QCOMPARE(done.at(1).at(1).toString(), QStringLiteral("unresolved"));
        QVERIFY(keys.readCreateStage(keys.lease(), attempt).value.has_value());
    }

    // Review finding 4576938727 fix half (b): because no check can ever prove
    // a negative, stopping to await a late channel is a human decision with a
    // named consequence: the channel may still appear later and would then be
    // permanently unreadable. discardChannelCreateStage only acts on an
    // explicit confirmation argument; without it nothing changes, and no
    // network round trip is even attempted (a discard proves nothing — it
    // abandons waiting). It removes the staged key record and frees the
    // create-budget slot the dormant quarantine held.
    void encryptedChannelCreationUserDiscardIsInformedAndFreesSlot()
    {
        ControllerMemorySecrets secrets; QTemporaryDir vaultDir; QVERIFY(vaultDir.isValid());
        NativeChatKeyVault keys(vaultDir.filePath("vault"), &secrets);
        bool opened = false;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { opened = value == NativeChatKeyVault::Failure::None; });
        QVERIFY(QTest::qWaitFor([&opened] { return opened; }, 2000));
        const auto attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const auto keyUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray groupKey(32, char('d'));
        NativeChatKeyVault::CreateStage seed{attempt, keyUuid, groupKey,
            QDateTime::currentMSecsSinceEpoch() - 3600000};
        QCOMPARE(keys.putCreateStage(keys.lease(), seed), NativeChatKeyVault::Failure::None);
        QCOMPARE(keys.markCreateStageDispatchAttempted(keys.lease(), attempt), NativeChatKeyVault::Failure::None);
        seed.clearSecrets();
        int stageGets = 0;
        EncryptedChannelCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.server.handler = [inner = test.fixture.server.handler, &attempt,
            &stageGets](const Request &request) -> Response {
            const auto target = QStringLiteral("/api/workspace/v1/messenger/streams/") + attempt;
            if (request.method == "GET" && request.target.path() == target) { ++stageGets; return {QJsonObject{}, 404}; }
            return inner(request);
        };
        auto &controller = test.fixture.controller;
        controller.useChatKeys(&keys);
        // An unconfirmed discard is refused and changes nothing.
        {
            QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
            controller.discardChannelCreateStage(attempt, false);
            QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
                "an unconfirmed discard never settled");
            QCOMPARE(done.at(0).at(0).toString(), attempt);
            QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("unresolved"));
        }
        auto kept = keys.readCreateStage(keys.lease(), attempt);
        QVERIFY2(kept.value.has_value(), "an unconfirmed discard destroyed the staged key");
        kept.value->clearSecrets();
        // Discarding an unknown attempt settles unresolved without a check.
        {
            QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
            controller.discardChannelCreateStage(QUuid::createUuid().toString(QUuid::WithoutBraces), true);
            QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
                "discarding an unknown attempt never settled");
            QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("unresolved"));
        }
        // The informed explicit discard: the stage (and its budget slot) are
        // removed by the human's warned decision, with no server round trip.
        const auto getsBefore = stageGets;
        QSignalSpy done(&controller, &NativeController::channelCreateStageResolved);
        controller.discardChannelCreateStage(attempt, true);
        QVERIFY2(QTest::qWaitFor([&] { return done.size() == 1; }, 5000),
            "the confirmed discard never settled");
        QCOMPARE(done.at(0).at(0).toString(), attempt);
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("discarded"));
        QCOMPARE(stageGets, getsBefore); // a discard is a decision, not a proof attempt
        QCOMPARE(keys.readCreateStage(keys.lease(), attempt).failure, NativeChatKeyVault::Failure::MissingKey);
        QVERIFY(controller.unresolvedChannelCreateStages().isEmpty());
    }

    // Review finding 4576938732 (medium, R8): the inventory and its two user
    // decisions must have a user-facing caller, or a budget-full user is
    // directed to resolve attempts that the application cannot show or
    // resolve. Source contract: the shipped Main.qml must call all three
    // controller entry points and its discard confirmation must name the
    // key-loss risk the user is warned about.
    void encryptedChannelCreationUnresolvedFlowIsWiredIntoTheUi()
    {
#ifdef WORKSPACE_NATIVE_MAIN_QML_SOURCE
        QFile file(QStringLiteral(WORKSPACE_NATIVE_MAIN_QML_SOURCE));
        QVERIFY2(file.open(QIODevice::ReadOnly),
            "Main.qml source contract: the shipped UI source must be readable");
        const auto text = QString::fromUtf8(file.readAll());
        QVERIFY2(text.contains("unresolvedChannelCreateStages("),
            "Main.qml never calls the unresolved create-stage inventory");
        QVERIFY2(text.contains("resolveChannelCreateStage("),
            "Main.qml never offers the user-invokable check of an unresolved attempt");
        QVERIFY2(text.contains("discardChannelCreateStage("),
            "Main.qml never offers the explicit informed discard of an unresolved attempt");
        QVERIFY2(text.contains("channelStageReviewButton"),
            "Main.qml has no entry point opening the unresolved-creations inventory");
        QVERIFY2(text.contains("channelStageDiscardConfirm"),
            "Main.qml has no explicit discard confirmation control");
        QVERIFY2(text.contains("never be read"),
            "Main.qml's discard confirmation never names the key-loss risk");
#else
        QSKIP("built without the Main.qml source contract path");
#endif
    }

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
        test.fixture.server.handler = fallback;
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_VERIFY(controller.authenticated()); QTRY_COMPARE(controller.streams().size(), 2);
        QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(controller.channelCreationForm().value("attemptId"), form.value("attemptId"));
        QCOMPARE(controller.channelCreationForm().value("memberUuids"), form.value("memberUuids"));
        test.postDelay = test.getDelay = test.topicDelay = test.addDelay = test.bindingDelay = test.catalogDelay = 0;
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

    void topicCreationPostsOnceConfirmsAndNavigatesAfterTheCatalog()
    {
        TopicCreationFixture test; auto &controller = test.fixture.controller;
        QVERIFY(test.fixture.start()); controller.setDraft("Existing draft");
        test.postDelay = 50; test.catalogDelay = 80;
        QSignalSpy created(&controller, &NativeController::topicCreated);
        QVERIFY(controller.canCreateTopicIn(streamId));
        controller.createTopic(streamId, "  Created topic  ");
        controller.createTopic(streamId, "Duplicate");
        QVERIFY(controller.topicCreationBusy());
        QCOMPARE(controller.topicCreationName(), "Created topic");
        QTRY_COMPARE(test.posts, 1);
        QVERIFY(isUuid(test.created.value("uuid").toString()));
        QTRY_COMPARE(test.confirmations, 1);
        QCOMPARE(created.size(), 0);
        QTRY_COMPARE(created.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.catalogs, 1);
        QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString());
        QCOMPARE(controller.topics().size(), 2);
        controller.selectTopic(topicId); QCOMPARE(controller.draft(), "Existing draft");
        for (const auto &request : test.fixture.server.requests)
            if (request.method == "POST" && request.target.path().endsWith("/stream_topics/")) {
                QCOMPARE(request.body.size(), 3);
                QCOMPARE(request.body.value("name").toString(), "Created topic");
                QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer synthetic-project-token"));
            }
    }

    void topicCreationNameValidationCountsUnicodeCodePoints_data()
    {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("valid");
        QTest::newRow("empty") << QString("  ") << false;
        QTest::newRow("129-ascii") << QString(129, 'a') << false;
        QTest::newRow("128-astral") << QString::fromUtf8("😀").repeated(128) << true;
        QTest::newRow("129-astral") << QString::fromUtf8("😀").repeated(129) << false;
    }
    void topicCreationNameValidationCountsUnicodeCodePoints()
    {
        QFETCH(QString, name); QFETCH(bool, valid);
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.controller.createTopic(streamId, name);
        if (valid) { QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!test.fixture.controller.topicCreationBusy()); }
        else {
            QCOMPARE(test.posts, 0); QVERIFY(!test.fixture.controller.topicCreationError().isEmpty());
            QCOMPARE(test.fixture.controller.topicCreationStream(), streamId);
            QCOMPARE(test.fixture.controller.topicCreationName(), name.trimmed());
        }
    }

    void topicCreationKeepsAmbiguousIdentityForCheckOnly()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = 503;
        auto &controller = test.fixture.controller;
        QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Committed but acknowledgement lost");
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(controller.topicCreationUncertain());
        const auto id = test.created.value("uuid").toString();
        controller.createTopic(streamId, "Do not repeat");
        test.getStatus = 404; controller.checkTopicCreation(); controller.checkTopicCreation();
        QTRY_COMPARE(test.confirmations, 1); QTRY_VERIFY(!controller.topicCreationBusy());
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(created.size(), 0); QCOMPARE(test.posts, 1);
        test.getStatus = 200; controller.checkTopicCreation();
        QTRY_COMPARE(created.size(), 1);
        QCOMPARE(test.confirmations, 2); QCOMPARE(test.posts, 1); QCOMPARE(controller.selectedTopic(), id);
    }

    void topicCreationRejectsInvalidCanonicalIdentity_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"uuid", "project_id", "stream_uuid", "user_uuid", "name", "is_done", "unread_count"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void topicCreationRejectsInvalidCanonicalIdentity()
    {
        QFETCH(QString, field);
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.alterGet = [field](QJsonObject &row) {
            row[field] = field == "name" ? QJsonValue(42) : field == "is_done" ? QJsonValue("false")
                : field == "unread_count" ? QJsonValue(-1) : QJsonValue(returnedId);
        };
        QSignalSpy created(&test.fixture.controller, &NativeController::topicCreated);
        test.fixture.controller.createTopic(streamId, "Validation");
        QTRY_COMPARE(test.confirmations, 1); QTRY_VERIFY(!test.fixture.controller.topicCreationBusy());
        QVERIFY(test.fixture.controller.topicCreationUncertain()); QCOMPARE(created.size(), 0);
        QCOMPARE(test.fixture.controller.selectedTopic(), topicId); QCOMPARE(test.catalogs, 0);
    }

    void topicCreationRouteChangeSuppressesDelayedNavigation()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postDelay = 100;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Route fence"); QTRY_COMPARE(test.posts, 1);
        controller.selectTopic({});
        QTRY_VERIFY(!controller.topicCreationBusy());
        QCOMPARE(controller.selectedTopic(), QString{}); QCOMPARE(created.size(), 0);
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(test.confirmations, 0);
        controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
    }

    void topicCreationDoesNotInventOwnerOrEncryptionRestrictions_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"encrypted", "direct", "provider", "guest", "member", "moderator", "administrator", "owner"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void topicCreationDoesNotInventOwnerOrEncryptionRestrictions()
    {
        QFETCH(QString, kind);
        TopicCreationFixture test;
        test.fixture.streams = [kind](const Request &) {
            const QSet<QString> roles{"guest", "member", "moderator", "administrator", "owner"};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Channel"},
                {"encryption", kind == "encrypted"}, {"role", roles.contains(kind) ? kind : QString("member")},
                {"direct_user_uuid", kind == "direct" ? QJsonValue(returnedId) : QJsonValue::Null},
                {"source_name", kind == "provider" ? "zulip" : "native"}}}};
        };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        controller.selectStream({}); QVERIFY(controller.canCreateTopicIn(streamId));
        QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Metadata only"); QTRY_COMPARE(created.size(), 1);
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString());
        QCOMPARE(test.posts, 1); QCOMPARE(test.catalogs, 1);
    }

    void topicCreationFromAnUnselectedStreamNavigatesOnlyAfterVerifiedCatalog()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.streams = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "First"}, {"encryption", false}},
                QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"name", "Target"}, {"encryption", false}}}};
        };
        auto &controller = test.fixture.controller; controller.refresh(); QTRY_COMPARE(controller.streams().size(), 2);
        QTRY_VERIFY(!controller.busy()); QSignalSpy created(&controller, &NativeController::topicCreated);
        test.catalogDelay = 80; controller.createTopic(returnedId, "Target topic"); QTRY_COMPARE(test.catalogs, 1);
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(created.size(), 0);
        QTRY_COMPARE(created.size(), 1); QCOMPARE(controller.selectedStream(), returnedId);
        QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString()); QCOMPARE(test.catalogs, 1);
    }

    void topicCreationPostFailuresHaveDistinctRetrySemantics_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("uncertain");
        for (const auto status : {400, 401, 403, 404, 408, 409, 429, 503})
            QTest::newRow(QByteArray::number(status)) << status << (status != 400 && status != 401 && status != 403 && status != 404);
    }
    void topicCreationPostFailuresHaveDistinctRetrySemantics()
    {
        QFETCH(int, status); QFETCH(bool, uncertain);
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = status;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Preserved input"); QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!controller.topicCreationBusy());
        QCOMPARE(controller.topicCreationUncertain(), uncertain); QVERIFY(!controller.topicCreationError().isEmpty());
        QCOMPARE(controller.topicCreationName(), "Preserved input"); QCOMPARE(test.confirmations, 0); QCOMPARE(created.size(), 0);
        const auto id = test.created.value("uuid").toString();
        test.postStatus = 201; controller.createTopic(streamId, "Explicit next attempt");
        if (uncertain) { QCOMPARE(test.posts, 1); QCOMPARE(test.created.value("uuid").toString(), id); }
        else { QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 2); QVERIFY(test.created.value("uuid").toString() != id); }
    }

    void topicCreationCanonicalGetIsFencedByLiveUpdateOrDeletion_data()
    {
        QTest::addColumn<bool>("deleted"); QTest::newRow("update") << false; QTest::newRow("delete") << true;
    }
    void topicCreationCanonicalGetIsFencedByLiveUpdateOrDeletion()
    {
        QFETCH(bool, deleted);
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.getDelay = 120;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Before live event"); QTRY_COMPARE(test.confirmations, 1);
        auto live = test.created; live["name"] = "Newer live name";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent(deleted ? "topic.deleted" : "topic.updated", live)));
        QTRY_VERIFY(!controller.topicCreationBusy()); QCOMPARE(created.size(), 0); QCOMPARE(controller.selectedTopic(), topicId);
        QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        if (deleted) {
            QVERIFY(!controller.topicCreationUncertain());
            controller.checkTopicCreation(); QCOMPARE(test.confirmations, 1);
            const auto topics = controller.topics();
            QVERIFY(std::none_of(topics.cbegin(), topics.cend(), [&](const QVariant &row) {
                return row.toMap().value("id").toString() == test.created.value("uuid").toString();
            }));
        } else {
            QVERIFY(controller.topicCreationUncertain()); test.created = live; test.getDelay = 0;
            controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1);
            QCOMPARE(controller.topics().last().toMap().value("name").toString(), "Newer live name");
        }
    }

    void topicCreationCatalogFailureRemainsCheckOnlyAndDoesNotHidePending()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.catalogStatus = 503;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Created, catalog unavailable"); QTRY_COMPARE(test.catalogs, 1);
        QTRY_VERIFY(!controller.topicCreationBusy()); QVERIFY(controller.topicCreationUncertain());
        QVERIFY(controller.topicCreationError().contains("created")); QCOMPARE(created.size(), 0);
        test.catalogStatus = 200; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 2); QCOMPARE(test.catalogs, 2);
    }

    void topicCreationCatalogPaginationIsCompleteAndCachedWithoutRedundantGet()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        TopicCreationFixture test; test.fixture.controller.useCache(&cache); QVERIFY(test.fixture.start());
        const auto fallback = test.fixture.server.handler;
        int pages = 0;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (test.posts && request.method == "GET" && request.target.path().endsWith("/stream_topics/")) {
                ++pages;
                if (QUrlQuery(request.target).queryItemValue("page_marker").isEmpty())
                    return {QJsonArray{creationTopic(topicId, streamId, "Native topic")}, 200, 0, {{"X-Pagination-Marker", "second-page"}}};
                return {QJsonArray{test.created}};
            }
            return fallback(request);
        };
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Paged topic"); QTRY_COMPARE(created.size(), 1); QCOMPARE(pages, 2);
        QVERIFY(flushCache(cache));
        const auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, streamId, {}, {}});
        QVERIFY(saved); QVERIFY(saved->fresh); QCOMPARE(saved->rows.size(), 2); QVERIFY(saved->nextMarker.isEmpty());
        QCOMPARE(saved->rows.last().toObject().value("name").toString(), QString("Paged topic"));
        QVERIFY(saved->rows.last().toObject().value("is_done").isBool());
        controller.selectStream({}); controller.selectStream(streamId); QTRY_COMPARE(controller.topics().size(), 2);
        QCOMPARE(pages, 2); // Current authoritative cache avoids another catalog request.
    }

    void topicCreationReentrantNavigationDoesNotStrandTheIntent()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy created(&controller, &NativeController::topicCreated);
        bool left = false;
        connect(&controller, &NativeController::topicsChanged, &controller, [&] {
            if (!left && test.posts && controller.topics().size() == 2 && controller.selectedTopic().isEmpty()) {
                left = true; controller.selectStream({});
            }
        });
        controller.createTopic(streamId, "Do not override navigation"); QTRY_VERIFY(left);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicCreationBusy(), 1000);
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(created.size(), 0); QCOMPARE(controller.selectedStream(), QString{});
    }

    void topicCreationSessionAndAclChangesFenceDelayedReplies_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"logout", "token", "archive", "role", "cache-replace"}) QTest::newRow(change) << QString::fromLatin1(change);
    }
    void topicCreationSessionAndAclChangesFenceDelayedReplies()
    {
        QFETCH(QString, change);
        QTemporaryDir first, second; NativeCacheService cache(first.path(), {}, true), replacement(second.path(), {}, true);
        TopicCreationFixture test; if (change == "cache-replace") test.fixture.controller.useCache(&cache);
        QVERIFY(test.fixture.start()); test.getDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Captured identity"); QTRY_COMPARE(test.confirmations, 1);
        const auto id = test.created.value("uuid").toString();
        if (change == "logout") controller.logout();
        else if (change == "token") {
            test.fixture.authentication = [](const Request &request) {
                if (request.target.path().endsWith("/actions/get_token/invoke"))
                    return Response{QJsonObject{{"access_token", "rotated-topic-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
                return sessionResponse(request);
            };
            test.fixture.session.requestRefresh();
        } else if (change == "cache-replace") controller.useCache(&replacement);
        else {
            auto row = change == "archive" ? creationChannel(streamId, "Changed")
                : QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Changed"}, {"encryption", false}};
            row[change == "archive" ? "is_archived" : "role"] = change == "archive" ? QJsonValue(true) : QJsonValue("new-role");
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
        }
        QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(created.size(), 0); QVERIFY(controller.selectedTopic() != id); QVERIFY(!controller.topicCreationBusy());
        QCOMPARE(test.posts, 1);
        if (change == "logout") { QVERIFY(test.fixture.start()); QVERIFY(controller.topicCreationUncertain()); }
        if (change == "token") {
            QTRY_VERIFY(!controller.busy()); QVERIFY(controller.topicCreationUncertain());
            test.getDelay = 0; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
        }
    }

    void topicCreationBoundsRequestsEvenForUniqueEmptyPages()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        const auto fallback = test.fixture.server.handler;
        int pages = 0;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (test.posts && request.method == "GET" && request.target.path().endsWith("/stream_topics/"))
                return {QJsonArray{}, 200, 0, {{"X-Pagination-Marker", QByteArray::number(++pages)}}};
            return fallback(request);
        };
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        QSignalSpy changed(&controller, &NativeController::stateChanged);
        controller.createTopic(streamId, "No pagination storm");
        QVERIFY2(changed.wait(), qPrintable(QString("Still pending after %1 empty HTTP pages").arg(pages)));
        QVERIFY(!controller.topicCreationBusy());
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(pages, 500); QCOMPARE(test.posts, 1); QCOMPARE(created.size(), 0);
    }

    void topicCreationDoesNotAcceptAnIncompatiblePostIdentity()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.alterPost = [](QJsonObject &row) { row["uuid"] = returnedId; };
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Frozen UUID"); QTRY_COMPARE(test.posts, 1);
        QTRY_VERIFY(controller.topicCreationUncertain()); QCOMPARE(test.confirmations, 0); QCOMPARE(created.size(), 0);
        controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
        QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString());
    }

    void topicCreationLiveCatalogRevisionRejectsAnOlderPage()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.catalogDelay = 100;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Old catalog name"); QTRY_COMPARE(test.catalogs, 1);
        test.created["name"] = "New live name";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", test.created)));
        QTRY_VERIFY(!controller.topicCreationBusy()); QVERIFY(controller.topicCreationUncertain()); QCOMPARE(created.size(), 0);
        QCOMPARE(controller.topics().last().toMap().value("name").toString(), "New live name");
        test.catalogDelay = 0; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1);
        QCOMPARE(controller.topics().last().toMap().value("name").toString(), "New live name"); QCOMPARE(test.posts, 1);
    }

    void topicCreationRejectsUnverifiedUnknownAndArchivedChannels()
    {
        TopicCreationFixture test;
        QVERIFY(!test.fixture.controller.canCreateTopicIn(streamId));
        test.fixture.controller.createTopic(streamId, "Signed out"); QCOMPARE(test.posts, 0);
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QVERIFY(!controller.canCreateTopicIn(returnedId));
        controller.createTopic(returnedId, "Unknown"); QCOMPARE(test.posts, 0);
        auto row = creationChannel(streamId, "Archived"); row["is_archived"] = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
        QVERIFY(!controller.canCreateTopicIn(streamId)); controller.createTopic(streamId, "Archived"); QCOMPARE(test.posts, 0);
    }

    void streamContextKeyReflectsOnlyVerifiedAccessMetadata()
    {
        Fixture fixture; QVERIFY(fixture.start());
        const auto key = fixture.controller.streams().first().toMap().value("contextKey").toString();
        QCOMPARE(key.size(), 64);
        auto row = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Renamed"}, {"encryption", false}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
        QCOMPARE(fixture.controller.streams().first().toMap().value("contextKey").toString(), key);
        row["role"] = "member"; QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", row, 13)));
        const auto changed = fixture.controller.streams().first().toMap().value("contextKey").toString();
        QVERIFY(changed != key); QCOMPARE(changed.size(), 64);
    }

    void topicCreationNavigationSubscribersCanDestroyTheController()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> current = new NativeController(&test.fixture.api, &test.fixture.session);
        current->refresh(); QTRY_VERIFY(!current->busy()); QTRY_COMPARE(current->streams().size(), 1);
        current->selectStream(streamId); QTRY_COMPARE(current->topics().size(), 1);
        connect(current, &NativeController::topicsChanged, &test.fixture.session, [&] {
            if (current && test.posts && current->topics().size() == 2) delete current.data();
        });
        current->createTopic(streamId, "Subscriber lifetime"); QTRY_VERIFY(current.isNull());
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 1);
    }

    void viewerNameFallsBackWhileDirectoryIsStaleAndBoundsUntrustedNames()
    {
        Fixture fixture;
        fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto profile = raw.value("user_info").toObject();
                profile["first_name"] = "Cassandra"; raw["user_info"] = profile; response.data = raw;
            }
            return response;
        };
        fixture.users = [](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", "cassi"}}}}; };
        QVERIFY(fixture.start()); QCOMPARE(fixture.controller.viewerDisplayName(), "cassi");
        fixture.users = [](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId},
            {"username", "cassi"}, {"first_name", QString(129, 'x')}}}, 200, 80}; };
        fixture.controller.refresh(); QCOMPARE(fixture.controller.viewerDisplayName(), "Cassandra");
        QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(fixture.controller.viewerDisplayName(), "Cassandra");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", userId},
            {"username", "cassi"}, {"first_name", "<b>Cassi</b>"}})));
        QCOMPARE(fixture.controller.viewerDisplayName(), "<b>Cassi</b>"); // Plain text projection, no markup evaluation.
        fixture.controller.logout(); QCOMPARE(fixture.controller.viewerDisplayName(), QString{});
    }

    void topicCreationUncertainOwnerCannotLeakIntoANewVerifiedAccount()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postDelay = 120;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Original owner intent"); QTRY_COMPARE(test.posts, 1);
        const auto attempt = controller.topicCreationAttemptId(), context = controller.topicCreationContextKey();
        QVERIFY(isUuid(attempt)); QCOMPARE(context.size(), 64);
        test.fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto user = raw.value("user_info").toObject();
                user["uuid"] = returnedId; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        QVERIFY(test.fixture.start()); QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QVERIFY(!controller.topicCreationBusy()); QVERIFY(!controller.topicCreationUncertain());
        QCOMPARE(controller.topicCreationStream(), QString{}); QCOMPARE(controller.topicCreationName(), QString{});
        QVERIFY(controller.topicCreationAttemptId().isEmpty()); QVERIFY(controller.topicCreationContextKey() != context);
        QCOMPARE(created.size(), 0); QVERIFY(controller.canCreateTopicIn(streamId));
        test.fixture.authentication = {}; QVERIFY(test.fixture.start()); QVERIFY(controller.topicCreationUncertain());
        QCOMPARE(controller.topicCreationAttemptId(), attempt);
        test.postDelay = 0; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
    }

    void createdTopicCatalogSurvivesAColdOfflineMessengerRestart()
    {
        QTemporaryDir directory; TopicCreationFixture test; QVERIFY(test.fixture.start());
        QString id;
        {
            NativeCacheService cache(directory.path(), {}, true);
            NativeController current(&test.fixture.api, &test.fixture.session); current.useCache(&cache);
            QTRY_VERIFY(!current.busy()); QTRY_COMPARE(current.streams().size(), 1);
            current.selectStream(streamId); QTRY_COMPARE(current.topics().size(), 1);
            QSignalSpy created(&current, &NativeController::topicCreated);
            current.createTopic(streamId, "Durable topic"); QTRY_COMPARE(created.size(), 1);
            id = test.created.value("uuid").toString(); QVERIFY(flushCache(cache));
        }
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback](const Request &request) {
            if (request.method == "GET" && (request.target.path().contains("/messenger/") || request.target.path().endsWith("/users/")))
                return Response{QJsonObject{}, 503};
            return fallback(request);
        };
        NativeCacheService cache(directory.path(), {}, true);
        ApiClient api(nullptr, true); Session session(&api); NativeController current(&api, &session); current.useCache(&cache);
        current.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId); QTRY_COMPARE(current.streams().size(), 1);
        current.selectStream(streamId); QTRY_COMPARE(current.topics().size(), 2);
        const auto restoredTopics = current.topics();
        QVERIFY(std::any_of(restoredTopics.cbegin(), restoredTopics.cend(), [&id](const QVariant &row) {
            return row.toMap().value("id").toString() == id && row.toMap().value("name").toString() == "Durable topic";
        }));
        QVERIFY(current.cacheStale()); QVERIFY(!current.canCreateTopicIn(streamId)); QCOMPARE(test.posts, 1);
    }

    void viewerDisplayNameUsesOnlyFreshOwnDirectoryNames()
    {
        Fixture fixture;
        fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto data = response.data.toObject(); auto profile = data.value("user_info").toObject();
                profile["first_name"] = "Cassandra"; data["user_info"] = profile; response.data = data;
            }
            return response;
        };
        fixture.users = [](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId},
            {"first_name", " Cassi "}, {"last_name", " Volkova "}, {"username", "cassi"}, {"name", "Ignore extension"}}}}; };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.viewerDisplayName(), "Cassi Volkova");
        QCOMPARE(fixture.controller.account().value("displayName").toString(), "Cassandra");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", returnedId},
            {"username", "foreign"}, {"first_name", "Another"}})));
        QCOMPARE(fixture.controller.viewerDisplayName(), "Cassi Volkova");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", userId},
            {"username", "cassi"}, {"first_name", "Cassi"}, {"last_name", "Updated"}})));
        QCOMPARE(fixture.controller.viewerDisplayName(), "Cassi Updated");
        fixture.controller.logout(); QCOMPARE(fixture.controller.viewerDisplayName(), QString{});
    }
};

QTEST_MAIN(ControllerCreationTest)
#include "native-controller-creation-test.moc"
