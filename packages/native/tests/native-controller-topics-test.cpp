#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"

class ControllerTopicsTest final : public NativeControllerTestHarness {
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

    // Issue #636 (docs/NATIVE_DESKTOP_TOPIC_CONTEXT_ACTIONS.md): topic context
    // menu actions live at the controller: applyTopicAction posts the same
    // transport as the drawer's applyChannelTopicAction (reference
    // runWorkspaceTopic* in messenger-sidebar-actions.lib.ts:1083-1121), with a
    // pending guard, an error state and per-topic arbitration only.
    //
    // RED first witness: applyTopicAction/topicActionBusy/topicActionState do
    // not exist yet, so this case fails to compile against the controller.
    void topicActionNotificationUpdateMapsModeUpdatesRowAndGuardsPending()
    {
        Fixture fixture;
        QJsonObject dto{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"name", "Native topic"}, {"unread_count", 3}, {"active_unread_count", 3},
            {"passive_unread_count", 0}, {"notification_mode", "default"}, {"is_done", false}};
        fixture.topics = [&](const Request &) { QJsonArray rows; rows.append(dto); return Response{rows}; };
        const auto base = fixture.server.handler;
        int posts = 0;
        QJsonObject postedBody;
        std::function<void()> release;
        bool deferNext = true;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path == "/api/workspace/v1/messenger/stream_topics/"
                + topicId + "/actions/notifications/invoke") {
                ++posts;
                postedBody = request.body;
                dto["notification_mode"] = request.body.value("notification_mode").toString();
                Response response{dto};
                if (deferNext) {
                    deferNext = false;
                    response.defer = [&](auto finish) { release = std::move(finish); };
                }
                return response;
            }
            return base(request);
        };
        QVERIFY(fixture.start());
        QSignalSpy finished(&fixture.controller, &NativeController::topicActionFinished);
        QSignalSpy changed(&fixture.controller, &NativeController::topicActionsChanged);
        fixture.controller.applyTopicAction(streamId, topicId, "notifications", "follow");
        QTRY_COMPARE(posts, 1);
        QCOMPARE(postedBody.value("notification_mode").toString(), QString("follow"));
        QVERIFY(fixture.controller.topicActionBusy(topicId));
        auto state = fixture.controller.topicActionState(streamId, topicId);
        QCOMPARE(state.value("notificationsPending").toBool(), true);
        // Pending guard: a second notification write for the same topic is
        // rejected without a second request (reference disables the row while
        // pending; a silently-dropped duplicate would hang the guard).
        fixture.controller.applyTopicAction(streamId, topicId, "notifications", "mute");
        QCOMPARE(posts, 1);
        QTRY_VERIFY(bool(release));
        release();
        QTRY_VERIFY(!fixture.controller.topicActionBusy(topicId));
        QTRY_VERIFY(changed.count() >= 2);
        bool sawOk = false;
        for (const auto &args : finished)
            if (args.at(0).toString() == streamId && args.at(1).toString() == topicId
                && args.at(2).toString() == "notifications" && args.at(3).toBool()) sawOk = true;
        QVERIFY(sawOk);
        state = fixture.controller.topicActionState(streamId, topicId);
        QCOMPARE(state.value("error").toString(), QString());
        QTRY_VERIFY(([&] {
            const auto rows = fixture.controller.sidebarTopicState(streamId, true).value("rows").toList();
            return std::any_of(rows.cbegin(), rows.cend(), [](const QVariant &row) {
                const auto map = row.toMap();
                return map.value("id").toString() == topicId && map.value("notificationMode").toString() == "follow";
            });
        })());
    }

    void topicActionReadRenameAndDoneToggleUseTheReferenceTransport()
    {
        Fixture fixture;
        QJsonObject dto{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"name", "Native topic"}, {"unread_count", 2}, {"active_unread_count", 2},
            {"passive_unread_count", 0}, {"notification_mode", "default"}, {"is_done", false}};
        fixture.topics = [&](const Request &) { QJsonArray rows; rows.append(dto); return Response{rows}; };
        const auto base = fixture.server.handler;
        int reads = 0, renames = 0, dones = 0;
        QJsonObject renameBody;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto prefix = QString("/api/workspace/v1/messenger/stream_topics/") + topicId;
            if (request.method == "POST" && path == prefix + "/actions/read/invoke") {
                ++reads;
                dto["unread_count"] = 0; dto["active_unread_count"] = 0; dto["passive_unread_count"] = 0;
                return Response{dto};
            }
            if (request.method == "PUT" && path == prefix) {
                ++renames;
                renameBody = request.body;
                dto["name"] = request.body.value("name").toString();
                return Response{dto};
            }
            if (request.method == "POST" && path == prefix + "/actions/toggle_done/invoke") {
                ++dones;
                dto["is_done"] = !dto.value("is_done").toBool();
                return Response{dto};
            }
            return base(request);
        };
        QVERIFY(fixture.start());
        // Empty rename never reaches the network (reference submitDisabled).
        fixture.controller.applyTopicAction(streamId, topicId, "rename", "   ");
        QCOMPARE(renames, 0);
        fixture.controller.applyTopicAction(streamId, topicId, "read", "");
        QTRY_COMPARE(reads, 1);
        const auto rowsOk = [&](const auto &pred) {
            const auto rows = fixture.controller.sidebarTopicState(streamId, true).value("rows").toList();
            return std::any_of(rows.cbegin(), rows.cend(), pred);
        };
        QTRY_VERIFY(!rowsOk([&](const QVariant &row) { const auto map = row.toMap(); return map.value("id").toString() == topicId
                && map.value("unread").toInt() > 0; }));
        fixture.controller.applyTopicAction(streamId, topicId, "rename", "  Renamed topic  ");
        QTRY_COMPARE(renames, 1);
        QCOMPARE(renameBody.value("name").toString(), QString("Renamed topic"));
        QTRY_VERIFY(rowsOk([&](const QVariant &row) { const auto map = row.toMap(); return map.value("id").toString() == topicId
                && map.value("name").toString() == "Renamed topic"; }));
        fixture.controller.applyTopicAction(streamId, topicId, "toggle_done", "");
        QTRY_COMPARE(dones, 1);
        QTRY_VERIFY(rowsOk([&](const QVariant &row) { const auto map = row.toMap(); return map.value("id").toString() == topicId
                && map.value("done").toBool(); }));
        fixture.controller.applyTopicAction(streamId, topicId, "toggle_done", "");
        QTRY_COMPARE(dones, 2);
        QTRY_VERIFY(rowsOk([&](const QVariant &row) { const auto map = row.toMap(); return map.value("id").toString() == topicId
                && !map.value("done").toBool(); }));
        QCOMPARE(fixture.controller.topicActionBusy(topicId), false);
    }

    void topicActionFailureReportsTheErrorStateWithoutHangingTheGuard()
    {
        Fixture fixture;
        const auto base = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (request.method == "POST" && request.target.path().endsWith("/actions/notifications/invoke"))
                return Response{QJsonObject{}, 500};
            return base(request);
        };
        QVERIFY(fixture.start());
        QSignalSpy finished(&fixture.controller, &NativeController::topicActionFinished);
        fixture.controller.applyTopicAction(streamId, topicId, "notifications", "follow");
        QTRY_VERIFY(!fixture.controller.topicActionBusy(topicId));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(3).toBool(), false);
        const auto state = fixture.controller.topicActionState(streamId, topicId);
        QCOMPARE(state.value("notificationsError").toString(),
            QString("Could not update the topic notifications. Please try again."));
        QCOMPARE(state.value("notificationsPending").toBool(), false);
    }

    void topicActionPendingOnOneTopicDoesNotBlockAnotherTopic()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        const auto base = fixture.server.handler;
        int posts = 0;
        std::function<void()> release;
        // The catalog reflects the last written mode: when the peer-topic write
        // settles through the reconcile-by-read (the existing revision-race
        // contract), the re-read shows the committed server truth.
        QHash<QString, QString> modes;
        fixture.topics = [&](const Request &) {
            QJsonArray rows;
            rows.append(QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Native topic"}, {"notification_mode", modes.value(topicId, "default")}});
            rows.append(QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Second topic"}, {"notification_mode", modes.value(secondTopicId, "default")}});
            return Response{rows};
        };
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path.contains("/actions/notifications/invoke")) {
                ++posts;
                const auto mode = request.body.value("notification_mode").toString();
                if (path.contains("/" + topicId + "/")) {
                    modes.insert(topicId, mode);
                    Response response{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                        {"stream_uuid", streamId}, {"name", "Native topic"}, {"notification_mode", mode}}};
                    response.defer = [&](auto finish) { release = std::move(finish); };
                    return response;
                }
                modes.insert(secondTopicId, mode);
                return Response{QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Second topic"}, {"notification_mode", mode}}};
            }
            return base(request);
        };
        QVERIFY(fixture.start());
        fixture.controller.applyTopicAction(streamId, topicId, "notifications", "follow");
        QTRY_VERIFY(bool(release));
        // A different topic is never blocked by a peer topic's write (pinned
        // drawer case openingAnotherTopicMenuDuringMuteAllowsFurtherActions).
        // The mock connection serializes replies, so acceptance is witnessed
        // by the pending guard; the delivery itself lands after the release.
        fixture.controller.applyTopicAction(streamId, secondTopicId, "notifications", "mute");
        QVERIFY(fixture.controller.topicActionBusy(secondTopicId));
        QVERIFY(fixture.controller.topicActionBusy(topicId));
        release();
        QTRY_COMPARE(posts, 2);
        QTRY_VERIFY(([&] {
            const auto rows = fixture.controller.sidebarTopicState(streamId, true).value("rows").toList();
            return std::any_of(rows.cbegin(), rows.cend(), [](const QVariant &row) {
                const auto map = row.toMap(); return map.value("id").toString() == secondTopicId
                    && map.value("notificationMode").toString() == "mute"; });
        })());
        QTRY_VERIFY(!fixture.controller.topicActionBusy(topicId));
        QTRY_VERIFY(!fixture.controller.topicActionBusy(secondTopicId));
    }
};

QTEST_MAIN(ControllerTopicsTest)
#include "native-controller-topics-test.moc"
