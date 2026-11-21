#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"
#include "support/controller-navigation-fixtures.h"

class ControllerNavigationTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void lastConversationStartupUsesCanonicalOwnedRoute_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"stream", "topic", "moved-topic"}) QTest::newRow(mode) << QString(mode);
    }
    void lastConversationStartupUsesCanonicalOwnedRoute()
    {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        TopicOpeningFixture test;
        const auto owner = conversationPreferenceOwner(test.fixture.server.origin());
        if (mode != "moved-topic") test.resolved["stream_uuid"] = streamId;
        else test.resolved["uuid"] = openingCanonicalId;
        {
            NativePreferences initial(path);
            initial.recordLastConversation(owner, streamId, mode == "stream" ? QString{} : openingRequestedId);
        }
        NativePreferences preferences(path);
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        const auto expectedStream = mode == "moved-topic" ? openingStreamId : streamId;
        const auto expectedTopic = mode == "stream" ? QString{} : test.resolved.value("uuid").toString();
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), expectedStream, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), expectedTopic, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
        QCOMPARE(test.lookups, mode == "stream" ? 0 : 1);
        QCOMPARE(test.mutations, 0);
        QCOMPARE(preferences.loadLastConversation(owner), (QStringList{expectedStream, expectedTopic}));
        QCOMPARE(test.historyTargets.size(), 1);
        for (const auto &target : test.historyTargets) {
            const QUrlQuery query(target);
            QCOMPARE(query.queryItemValue("stream_uuid"), expectedStream);
            QCOMPARE(query.queryItemValue("topic_uuid"), expectedTopic);
        }
    }

    void manualNavigationWinsBeforeAutomaticConversationDispatch_data()
    {
        QTest::addColumn<bool>("root");
        QTest::newRow("manual-stream") << false;
        QTest::newRow("manual-messenger-root") << true;
    }
    void manualNavigationWinsBeforeAutomaticConversationDispatch()
    {
        QFETCH(bool, root);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test;
        const auto owner = conversationPreferenceOwner(test.fixture.server.origin());
        preferences.recordLastConversation(owner, streamId, openingRequestedId);
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        bool chosen = false;
        connect(&controller, &NativeController::streamsChanged, &controller, [&] {
            if (chosen || !controller.authenticated() || controller.streams().isEmpty()) return;
            chosen = true;
            controller.selectStream(root ? QString{} : streamId);
        });
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_VERIFY(chosen);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(controller.selectedStream(), root ? QString{} : streamId);
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.mutations, 0);
        QCOMPARE(preferences.loadLastConversation(owner), (QStringList{root ? QString{} : streamId, QString{}}));
    }

    void lastConversationDoesNotCrossTheVerifiedOwner()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test;
        preferences.recordLastConversation(conversationPreferenceOwner(test.fixture.server.origin(), returnedId), streamId, openingRequestedId);
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        QTRY_VERIFY(!controller.busy());
        QVERIFY(controller.selectedStream().isEmpty());
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.mutations, 0);
    }

    void unavailableSavedConversationFallsBackQuietly_data()
    {
        QTest::addColumn<QString>("missing");
        for (const auto *kind : {"stream", "topic-403", "topic-404", "destination-403", "destination-404"})
            QTest::newRow(kind) << QString(kind);
    }

    void unavailableSavedConversationFallsBackQuietly()
    {
        QFETCH(QString, missing);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test;
        const auto owner = conversationPreferenceOwner(test.fixture.server.origin());
        preferences.recordLastConversation(owner, openingStreamId,
            missing == "stream" ? QString{} : openingRequestedId);
        if (missing.startsWith("topic")) test.lookupStatus = missing.endsWith("403") ? 403 : 404;
        if (missing.startsWith("destination")) test.detailStatus = missing.endsWith("403") ? 403 : 404;
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        if (missing != "stream") QTRY_COMPARE(test.lookups, 1);
        if (missing.startsWith("destination")) QTRY_COMPARE(test.details, 1);
        QTRY_VERIFY(!controller.busy() && !controller.topicResolving());
        // Flush the queued one-shot restoration after catalog publication.
        QTest::qWait(50);
        QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
        QVERIFY(controller.selectedStream().isEmpty());
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(preferences.loadLastConversation(owner), (QStringList{QString{}, QString{}}));
        QVERIFY(controller.inboxVisible());
        QCOMPARE(test.mutations, 0);
    }

    void queuedStreamLinkOpensFromStartupInbox_data()
    {
        QTest::addColumn<QString>("saved");
        QTest::newRow("missing-stream") << QString("missing");
        QTest::newRow("no-saved-conversation") << QString("none");
        QTest::newRow("existing-stream-control") << QString("existing");
        QTest::newRow("slow-topic-restore") << QString("slow-topic");
    }

    void queuedStreamLinkOpensFromStartupInbox()
    {
        QFETCH(QString, saved);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test;
        const auto owner = conversationPreferenceOwner(test.fixture.server.origin());
        if (saved != "none")
            preferences.recordLastConversation(owner, saved == "missing" ? openingStreamId : streamId,
                saved == "slow-topic" ? openingRequestedId : QString{});
        if (saved == "slow-topic") test.lookupDelay = 1100;
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        const auto origin = test.fixture.server.origin();
        controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/stream/" + streamId);
        controller.login(origin.toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        if (saved == "slow-topic") {
            QTRY_COMPARE(test.lookups, 1);
            QTest::qWait(850);
            QVERIFY(controller.topicResolving());
            QVERIFY(controller.selectedStream().isEmpty());
        }
        QTRY_VERIFY(!controller.busy() && !controller.topicResolving());
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), streamId, 1500);
        QVERIFY(controller.error().isEmpty());
        QCOMPARE(test.mutations, 0);
    }

    void automaticConversationRestorePreservesTransientErrorsAndExplicitOpenErrors_data()
    {
        QTest::addColumn<bool>("automatic"); QTest::addColumn<int>("status");
        QTest::newRow("automatic-offline") << true << 503;
        QTest::newRow("explicit-missing") << false << 404;
        QTest::newRow("explicit-denied") << false << 403;
    }

    void automaticConversationRestorePreservesTransientErrorsAndExplicitOpenErrors()
    {
        QFETCH(bool, automatic); QFETCH(int, status);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test; test.lookupStatus = status;
        const auto owner = conversationPreferenceOwner(test.fixture.server.origin());
        auto &controller = test.fixture.controller;
        if (automatic) preferences.recordLastConversation(owner, streamId, openingRequestedId);
        controller.usePreferences(&preferences);
        if (automatic) {
            controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        } else {
            QVERIFY(test.start()); controller.openTopic(openingRequestedId, streamId);
        }
        QTRY_COMPARE(test.lookups, 1);
        QTRY_VERIFY(!controller.topicResolving());
        QVERIFY(!controller.error().isEmpty());
        if (automatic) QCOMPARE(preferences.loadLastConversation(owner), (QStringList{streamId, openingRequestedId}));
        else QCOMPARE(controller.selectedTopic(), topicId);
    }

    void automaticConversationRestoreCannotReopenAnInboxTheUserLeft()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test; test.lookupStatus = 404; test.lookupDelay = 150;
        auto &controller = test.fixture.controller;
        preferences.recordLastConversation(conversationPreferenceOwner(test.fixture.server.origin()), streamId, openingRequestedId);
        controller.usePreferences(&preferences);
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_COMPARE(test.lookups, 1); QVERIFY(controller.topicResolving());
        controller.showInbox(); controller.closeInbox();
        QTRY_VERIFY(!controller.topicResolving()); QTest::qWait(200);
        QVERIFY(!controller.inboxVisible()); QVERIFY(controller.error().isEmpty());
    }

    void lastConversationReturnsAfterSameAccountSignsInAgain()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test; auto &controller = test.fixture.controller;
        test.resolved = creationTopic(secondTopicId, streamId, "Other known topic");
        test.additionalLookups.insert("/api/workspace/v1/messenger/stream_topics/" + secondTopicId, Response{test.resolved});
        controller.usePreferences(&preferences); QVERIFY(test.start()); controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!controller.busy()); controller.logout(); QVERIFY(!controller.authenticated());
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_COMPARE(controller.selectedTopic(), secondTopicId); QCOMPARE(controller.selectedStream(), streamId);
        QTRY_VERIFY(!controller.busy() && !controller.topicResolving()); QVERIFY(controller.error().isEmpty());
    }

    void canonicalTopicOpeningUsesOwnedReturnedIdentity_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"same-stream", "known-stream-move", "missing-stream-canonical-uuid",
                                "direct-read", "encrypted-read", "archived-read", "provider-read"})
            QTest::newRow(mode) << QString(mode);
    }
    void canonicalTopicOpeningUsesOwnedReturnedIdentity()
    {
        QFETCH(QString, mode);
        TopicOpeningFixture test;
        const bool same = mode == "same-stream";
        const bool known = mode == "known-stream-move";
        test.includeDestination = known;
        test.resolved["stream_uuid"] = same ? streamId : openingStreamId;
        if (mode == "missing-stream-canonical-uuid") test.resolved["uuid"] = openingCanonicalId;
        if (mode == "direct-read") { test.destination["private"] = true; test.destination["direct_user_uuid"] = channelMemberA; }
        if (mode == "encrypted-read") { test.destination["encryption"] = true; test.destination["current_encryption_key_uuid"] = channelMemberA; }
        if (mode == "archived-read") test.destination["is_archived"] = true;
        if (mode == "provider-read") { test.destination["source_name"] = "zulip"; test.destination["provider_uuid"] = channelMemberA; test.destination["provider_identity"] = "synthetic-provider"; }
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId, streamId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), test.resolved.value("uuid").toString(), 2000);
        QCOMPARE(controller.selectedStream(), test.resolved.value("stream_uuid").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.topicsComplete(), 2000);
        QCOMPARE(test.details, same || known ? 0 : 1);
        QCOMPARE(test.targetTopicLists, 1);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.catalogs, 0);
        QCOMPARE(test.users, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QCOMPARE(test.mutations, 0);
        QCOMPARE(test.historyTargets.size(), 1);
        const QUrlQuery history(test.historyTargets.first());
        QCOMPARE(history.queryItemValue("stream_uuid"), controller.selectedStream());
        QCOMPARE(history.queryItemValue("topic_uuid"), controller.selectedTopic());
        QVERIFY(!controller.canSend()); // Opening must not synthesize a default composer draft.
    }

    void canonicalTopicOpeningRejectsUnownedOrInvalidMetadata_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto *fault : {"viewer", "project", "uuid", "stream", "boolean", "401", "403", "404", "503"})
            QTest::newRow(fault) << QString(fault);
    }
    void canonicalTopicOpeningRejectsUnownedOrInvalidMetadata()
    {
        QFETCH(QString, fault);
        TopicOpeningFixture test;
        bool status = false;
        const auto code = fault.toInt(&status);
        if (status) test.lookupStatus = code;
        test.alterLookup = [&](QJsonObject &row) {
            if (fault == "viewer") row["user_uuid"] = channelMemberA;
            if (fault == "project") row["project_id"] = channelMemberA;
            if (fault == "uuid") row["uuid"] = "not-a-uuid";
            if (fault == "stream") row["stream_uuid"] = "not-a-stream";
            if (fault == "boolean") row["is_done"] = "false";
        };
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setDraft("Retain the original draft");
        controller.openTopic(openingRequestedId, openingStreamId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving(), 2000);
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.draft(), "Retain the original draft");
        QCOMPARE(test.details, 0);
        QCOMPARE(test.catalogs, 0);
        QCOMPARE(test.users, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QCOMPARE(test.aggregateHistory + test.topicHistory, 0);
        QCOMPARE(test.mutations, 0);
    }

    void newerKnownTopicOpeningPhysicallyCancelsTheOldLookup()
    {
        TopicOpeningFixture test;
        test.lookupDelay = 30000;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId, openingStreamId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QPointer<QNetworkReply> old;
        for (auto *reply : test.fixture.api.findChildren<QNetworkReply *>())
            if (reply->url().path().endsWith(openingRequestedId)) old = reply;
        QVERIFY(old);
        controller.openTopic(secondTopicId, openingStreamId);
        QTRY_VERIFY_WITH_TIMEOUT(!old, 2000);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), secondTopicId);
        QVERIFY(!controller.topicResolving());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.lookups, 1);
        QVERIFY(controller.error().isEmpty());
    }

    void apiGenerationChangeClearsTopicResolvingWithoutOldCallback()
    {
        TopicOpeningFixture test;
        test.lookupDelay = 500;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QVERIFY(controller.topicResolving());
        const auto generation = test.fixture.api.generation();
        test.fixture.api.setAccessToken("synthetic-rotated-token");
        QVERIFY(test.fixture.api.generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving(), 2000);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.mutations, 0);
    }

    void failedPartialTopicCatalogKeepsOwnedMessagesVisibleButCannotAuthorizeWrites_data()
    {
        QTest::addColumn<bool>("withCache");
        QTest::newRow("without-cache") << false;
        QTest::newRow("with-cache") << true;
    }
    void failedPartialTopicCatalogKeepsOwnedMessagesVisibleButCannotAuthorizeWrites()
    {
        QFETCH(bool, withCache);
        QTemporaryDir directory;
        std::unique_ptr<NativeCacheService> cache;
        TopicOpeningFixture test;
        FileClient files(&test.fixture.api);
        if (withCache) {
            cache = std::make_unique<NativeCacheService>(directory.path(), NativeCache::Policy{}, true);
            test.fixture.controller.useCache(cache.get());
        }
        test.fixture.controller.useFiles(&files);
        test.topicsStatus = 503;
        auto row = message("Owned HTTP message stays visible", returnedId, openingRequestedId);
        row["stream_uuid"] = openingStreamId;
        row["read"] = false;
        test.history = {row};
        QVERIFY(test.start());
        if (cache) { QTRY_VERIFY_WITH_TIMEOUT(cache->available(), 2000); QVERIFY(flushCache(*cache)); }
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), openingRequestedId, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.messages().size(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QVERIFY(!controller.topicsComplete());
        QCOMPARE(modelMessage(controller, returnedId).value("content").toString(), "Owned HTTP message stays visible");
        controller.setDraft("A partial topic row cannot authorize this draft");
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.canAttach());
        QVERIFY(!modelMessage(controller, returnedId).value("canReact").toBool());
        controller.send();
        controller.markMessagesReadUpTo(returnedId);
        controller.markVisibleMessagesRead(QVariantList{returnedId});
        controller.starMessage(returnedId);
        controller.editMessage(returnedId, "Do not send this edit");
        controller.deleteMessage(returnedId);
        QCOMPARE(test.mutations, 0);
        controller.copyMessage(returnedId);
        QCOMPARE(QGuiApplication::clipboard()->text(), "Owned HTTP message stays visible");
        if (cache) {
            QVERIFY(flushCache(*cache));
            const auto page = cacheEntry(*cache, cache->lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}});
            QVERIFY(!page || !page->fresh);
        }
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 1);
        QCOMPARE(test.targetTopicLists, 1);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.aggregateHistory, 0);
    }

    void selectedDeletedTopicResolutionKeepsOwnedNavigationAndWriteBarriers_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"canonical-owned-alias", "newer-known-topic-wins", "logout-cancels",
                                    "404-keeps-unresolved-topic-locked", "foreign-viewer-keeps-unresolved-topic-locked"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void selectedDeletedTopicResolutionKeepsOwnedNavigationAndWriteBarriers()
    {
        QFETCH(QString, scenario);
        // These handler captures outlive Fixture and every deferred send closure.
        std::function<void()> releaseLookup;
        bool retired = false;
        int uploads = 0;
        TopicOpeningFixture test;
        FileClient files(&test.fixture.api);
        auto &controller = test.fixture.controller;
        controller.useFiles(&files);
        test.includeDestination = true;
        test.resolved = creationTopic(openingCanonicalId, openingStreamId, "Canonical merged topic");
        auto canonicalMessage = message("Actual canonical alias history", returnedId, openingCanonicalId);
        canonicalMessage["stream_uuid"] = openingStreamId;
        test.history = QJsonArray{canonicalMessage};
        test.fixture.files = [&uploads](const Request &request) -> Response {
            if (request.method == "POST") ++uploads;
            return {QJsonObject{}, 503}; // Any upload here is an actual guard violation.
        };
        const QString lookupPath = "/api/workspace/v1/messenger/stream_topics/" + topicId;
        auto raw = test.resolved;
        if (scenario.startsWith("foreign-viewer")) raw["user_uuid"] = channelMemberA;
        Response lookup{scenario.startsWith("404-") ? QJsonObject{} : raw,
                        scenario.startsWith("404-") ? 404 : 200};
        lookup.defer = [&releaseLookup](std::function<void()> send) { releaseLookup = std::move(send); };
        test.additionalLookups.insert(lookupPath, lookup);
        // After the admitted deletion, a fresh origin list must not resurrect T.
        // Only this new test's exact origin-list response changes; legacy Fixture
        // and destination-list/canonical-history handling remain untouched.
        const auto previous = test.fixture.server.handler;
        test.fixture.server.handler = [previous, &retired](const Request &request) -> Response {
            if (retired && request.method == "GET"
                && request.target.path() == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == streamId)
                return {QJsonArray{creationTopic(secondTopicId, streamId, "Other known topic")}};
            return previous(request);
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile input(directory.filePath("canonical-alias-write-barrier.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write("synthetic"), qint64(9)); input.close();
        const QVariantList selection{QUrl::fromLocalFile(input.fileName())};
        QVERIFY(test.start());
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
        QVERIFY(controller.topicsComplete());
        controller.setDraft("Original topic draft must not be sent after deletion");
        QVERIFY(controller.canSend()); QVERIFY(controller.canAttach()); // Non-vacuous capability baseline.
        QCOMPARE(test.lookups, 0); QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);

        retired = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", topicId}, {"stream_uuid", streamId}}, 77)));
        // The event, not a direct openTopic call, must start the exact resolver.
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QCOMPARE(test.lookupTargets, QStringList{lookupPath});
        QVERIFY(releaseLookup);
        QPointer<QNetworkReply> pending;
        for (auto *reply : test.fixture.api.findChildren<QNetworkReply *>())
            if (reply->url().path() == lookupPath) pending = reply;
        QVERIFY(pending); QVERIFY(!pending->isFinished());
        QSignalSpy finished(pending.data(), &QNetworkReply::finished); QVERIFY(finished.isValid());
        QCOMPARE(finished.size(), 0); QVERIFY(controller.topicResolving());
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.draft(), QString("Original topic draft must not be sent after deletion"));
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        controller.send(); controller.attachFiles(selection);
        QVERIFY(!controller.fileBusy()); QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);
        QCOMPARE(test.aggregateHistory + test.topicHistory, 0);

        const bool latest = scenario == "newer-known-topic-wins", logout = scenario == "logout-cancels";
        if (latest) {
            controller.selectTopic(secondTopicId);
            QTRY_VERIFY_WITH_TIMEOUT(!pending, 2000); // Existing physical-cancellation contract.
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), secondTopicId);
            controller.setDraft("New explicit route draft");
            QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        } else if (logout) {
            controller.logout();
            QTRY_VERIFY_WITH_TIMEOUT(!pending, 2000);
            QVERIFY(!controller.authenticated()); QVERIFY(!controller.topicResolving());
            QVERIFY(controller.selectedStream().isEmpty()); QVERIFY(controller.selectedTopic().isEmpty());
            QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        }
        // Exactly one immutable response delivery, also safe after socket cancel.
        auto deliver = std::move(releaseLookup); releaseLookup = {}; QVERIFY(deliver); deliver();
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(finished.size(), 1);
        const bool invalid = scenario.startsWith("404-") || scenario.startsWith("foreign-viewer");
        if (latest) {
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), secondTopicId);
            QCOMPARE(controller.draft(), QString("New explicit route draft"));
            QCOMPARE(test.topicHistory, 1); QCOMPARE(test.targetTopicLists, 0);
        } else if (logout) {
            QVERIFY(!controller.authenticated()); QVERIFY(controller.selectedStream().isEmpty());
            QVERIFY(controller.selectedTopic().isEmpty()); QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
            QCOMPARE(test.topicHistory, 0); QCOMPARE(test.targetTopicLists, 0);
        } else if (invalid) {
            QVERIFY(!controller.topicResolving()); QVERIFY(!controller.error().isEmpty());
            // Source retains the unresolved explicit route; it does not silently
            // turn a permanently deleted/foreign T into an aggregate selection.
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
            QCOMPARE(controller.draft(), QString("Original topic draft must not be sent after deletion"));
            QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
            controller.send(); controller.attachFiles(selection);
            QVERIFY(!controller.fileBusy());
            QCOMPARE(test.topicHistory, 0); QCOMPARE(test.targetTopicLists, 0);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), openingStreamId, 2000);
            QCOMPARE(controller.selectedTopic(), openingCanonicalId);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
            QVERIFY(controller.topicsComplete());
            QCOMPARE(controller.messages().size(), 1);
            QCOMPARE(modelMessage(controller, returnedId).value("content").toString(), QString("Actual canonical alias history"));
            QCOMPARE(test.topicHistory, 1); QCOMPARE(test.targetTopicLists, 1);
            controller.setDraft("Fresh canonical route draft");
            QVERIFY(controller.canSend()); QVERIFY(controller.canAttach()); // Authority returns only on owned complete target.
            QVERIFY(controller.error().isEmpty());
        }
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(test.lookups, 1); QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.details, 0); QCOMPARE(test.catalogs, 0); QCOMPARE(test.users, 0);
        QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join(QStringLiteral("; "))));
        QCOMPARE(test.historyTargets.size(), logout || invalid ? 0 : 1);
        for (const auto &target : test.historyTargets) {
            const QUrlQuery query(target);
            QCOMPARE(query.queryItemValue("stream_uuid"), latest ? streamId : openingStreamId);
            QCOMPARE(query.queryItemValue("topic_uuid"), latest ? secondTopicId : openingCanonicalId);
        }
    }

    void selectedDeletedTopicBarrierPrecedesPublicationAndKeepsLatestIntent_data()
    {
        QTest::addColumn<bool>("selectNewer");
        QTest::newRow("first-publication-is-locked-then-404-and-live") << false;
        QTest::newRow("first-publication-selects-a-newer-known-topic") << true;
    }

    void selectedDeletedTopicBarrierPrecedesPublicationAndKeepsLatestIntent()
    {
        QFETCH(bool, selectNewer);
        std::function<void()> releaseLookup;
        bool retired = false, armed = false, observed = false;
        QString firstSignal, observedStream, observedTopic, observedDraft;
        int observedRows = -1, uploads = 0;
        bool observedSend = true, observedAttach = true;
        TopicOpeningFixture test;
        auto &controller = test.fixture.controller;
        FileClient files(&test.fixture.api);
        controller.useFiles(&files);
        const QString lookupPath = "/api/workspace/v1/messenger/stream_topics/" + topicId;
        Response lookup{QJsonObject{}, 404};
        lookup.defer = [&releaseLookup](std::function<void()> send) { releaseLookup = std::move(send); };
        test.additionalLookups.insert(lookupPath, lookup);
        auto oldMessage = message("Actual old selected-topic content", messageId, topicId);
        const auto previous = test.fixture.server.handler;
        test.fixture.server.handler = [previous, oldMessage, &retired](const Request &request) -> Response {
            auto response = previous(request); // Keep the real fixture's HTTP accounting.
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == streamId
                && QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId)
                response.data = QJsonArray{oldMessage};
            if (retired && request.method == "GET"
                && request.target.path() == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == streamId)
                response.data = QJsonArray{creationTopic(secondTopicId, streamId, "Other known topic")};
            return response;
        };
        test.fixture.files = [&uploads](const Request &request) -> Response {
            if (request.method == "POST") ++uploads;
            return {QJsonObject{}, 503};
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile input(directory.filePath("first-notify-write-barrier.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write("synthetic"), qint64(9)); input.close();
        const QVariantList selection{QUrl::fromLocalFile(input.fileName())};
        QVERIFY(test.start());
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(controller.messages().size(), 1);
        QCOMPARE(modelMessage(controller, messageId).value("content").toString(), QString("Actual old selected-topic content"));
        controller.setDraft("Keep the explicit original draft");
        QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        const auto observer = [&](const QString &signal) {
            if (!armed) return;
            armed = false; observed = true; firstSignal = signal;
            observedStream = controller.selectedStream(); observedTopic = controller.selectedTopic();
            observedDraft = controller.draft(); observedRows = controller.messages().size();
            observedSend = controller.canSend(); observedAttach = controller.canAttach();
            controller.send(); controller.attachFiles(selection);
            if (selectNewer) controller.selectTopic(secondTopicId);
        };
        const auto topics = connect(&controller, &NativeController::topicsChanged, &controller,
            [&] { observer(QStringLiteral("topicsChanged")); });
        const auto messages = connect(&controller, &NativeController::messagesChanged, &controller,
            [&] { observer(QStringLiteral("messagesChanged")); });
        const auto state = connect(&controller, &NativeController::stateChanged, &controller,
            [&] { observer(QStringLiteral("stateChanged")); });
        const auto snapshot = connect(&controller, &NativeController::snapshotFailed, &controller,
            [&] { observer(QStringLiteral("snapshotFailed")); });
        const auto disconnectObservers = qScopeGuard([&] {
            disconnect(topics); disconnect(messages); disconnect(state); disconnect(snapshot);
        });
        retired = armed = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", topicId}, {"stream_uuid", streamId}}, 78)));
        QVERIFY(observed); QVERIFY(!firstSignal.isEmpty());
        QCOMPARE(observedStream, streamId); QCOMPARE(observedTopic, topicId);
        QCOMPARE(observedDraft, QString("Keep the explicit original draft"));
        QCOMPARE(observedRows, 0); QVERIFY(!observedSend); QVERIFY(!observedAttach);
        QVERIFY(!controller.fileBusy()); QCOMPARE(uploads, 0); QCOMPARE(test.mutations, 0);
        if (selectNewer) {
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && test.topicHistory == 1, 2000);
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), secondTopicId);
            QCOMPARE(test.lookups, 0); QVERIFY(!releaseLookup); QVERIFY(!controller.topicResolving());
            controller.setDraft("New explicit user intent"); QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000); QVERIFY(releaseLookup);
            QCOMPARE(test.lookupTargets, QStringList{lookupPath});
            QVERIFY(controller.topicResolving()); QCOMPARE(controller.messages().size(), 0);
            auto deliver = std::move(releaseLookup); releaseLookup = {}; deliver();
            QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
            QVERIFY(!controller.topicResolving()); QVERIFY(!controller.error().isEmpty());
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("message.created", oldMessage, 79)));
            QCOMPARE(controller.messages().size(), 0); // Admitted real RT cannot refill the retired route.
            QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
            QCOMPARE(test.topicHistory, 0);
        }
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(test.aggregateHistory, 0); QCOMPARE(test.catalogs, 0); QCOMPARE(test.details, 0);
        QCOMPARE(test.users, 0); QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join(QStringLiteral("; "))));
    }

    void selectedTopicFollowsItsOwnedRealtimeMoveWithoutResolverGet()
    {
        TopicOpeningFixture test;
        test.includeDestination = true;
        test.resolved = creationTopic(topicId, openingStreamId, "Moved origin topic");
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setDraft("Draft remains under its original conversation");
        test.canonicalSeen = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", test.resolved, 25)));
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), openingStreamId, 2000);
        QCOMPARE(controller.selectedTopic(), topicId);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.mutations, 0);
    }

    void invalidTopicOpeningAndExistingSelectTopicRemainKnownOnly()
    {
        TopicOpeningFixture test;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.selectTopic(openingRequestedId);
        QCOMPARE(controller.selectedTopic(), topicId);
        controller.openTopic("not-a-topic", openingStreamId);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.mutations, 0);
    }


    void canonicalDestinationRejectsForeignOrIncompleteStreamMetadata_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto *fault : {"viewer", "project", "uuid", "boolean", "missing-owner", "401", "403", "404", "503"})
            QTest::newRow(fault) << QString(fault);
    }
    void canonicalDestinationRejectsForeignOrIncompleteStreamMetadata()
    {
        QFETCH(QString, fault);
        TopicOpeningFixture test;
        bool status = false;
        const auto code = fault.toInt(&status);
        if (status) test.detailStatus = code;
        if (fault == "viewer") test.destination["user_uuid"] = channelMemberA;
        if (fault == "project") test.destination["project_id"] = channelMemberA;
        if (fault == "uuid") test.destination["uuid"] = channelMemberA;
        if (fault == "boolean") test.destination["encryption"] = "false";
        if (fault == "missing-owner") test.destination.remove("owner");
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setDraft("Retain this draft after untrusted destination metadata");
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.details, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving(), 2000);
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.draft(), "Retain this draft after untrusted destination metadata");
        QCOMPARE(controller.streams().size(), 1);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.targetTopicLists + test.topicHistory + test.aggregateHistory, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void topicOpeningFixtureRecordsTheActualWorkspaceDirectoryAndUnexpectedRoutes()
    {
        TopicOpeningFixture test;
        QVERIFY(test.start());
        int completed = 0;
        test.fixture.api.request("GET", "/api/workspace/v1/users/", {}, {},
            [&](const ApiResult &result) { QVERIFY(result.ok()); ++completed; });
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 2000);
        QCOMPARE(test.users, 1);
        QVERIFY(test.unexpectedRequests.isEmpty());
        test.fixture.api.request("GET", "/api/workspace/v1/unexpected-topic-opening-probe", {}, {},
            [&](const ApiResult &result) { QCOMPARE(result.status, 404); ++completed; });
        QTRY_COMPARE_WITH_TIMEOUT(completed, 2, 2000);
        QCOMPARE(test.users, 1);
        QCOMPARE(test.unexpectedRequests,
            QStringList{"GET /api/workspace/v1/unexpected-topic-opening-probe"});
        QCOMPARE(test.mutations, 0);
    }

    void duplicateOpeningAndCurrentKnownCancellationHaveNotifiedState()
    {
        TopicOpeningFixture test;
        test.lookupDelay = 30000;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        QList<bool> resolvingStates;
        QObject observer;
        connect(&controller, &NativeController::stateChanged, &observer,
            [&] { resolvingStates.append(controller.topicResolving()); });
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QVERIFY(resolvingStates.contains(true));
        const auto held = test.openingReply();
        QVERIFY(held);
        controller.openTopic(openingRequestedId, openingStreamId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.openingReply(), held);
        controller.openTopic(topicId, openingStreamId);
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QVERIFY(!controller.topicResolving());
        QVERIFY(!resolvingStates.isEmpty());
        QCOMPARE(resolvingStates.last(), false);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.topicHistory, 1); // Existing selectTopic deliberately reloads even this topic.
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(controller.error().isEmpty());
    }

    void latestUnknownOpeningCancelsEachEarlierLookupStage_data()
    {
        QTest::addColumn<bool>("detail");
        QTest::newRow("topic-lookup") << false;
        QTest::newRow("stream-detail") << true;
    }
    void latestUnknownOpeningCancelsEachEarlierLookupStage()
    {
        QFETCH(bool, detail);
        TopicOpeningFixture test;
        if (detail) test.detailDelay = 30000; else test.lookupDelay = 30000;
        const auto secondPath = "/api/workspace/v1/messenger/stream_topics/" + openingCanonicalId;
        auto second = creationTopic(openingCanonicalId, streamId, "Latest canonical topic");
        test.additionalLookups.insert(secondPath, Response{second});
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        if (detail) QTRY_COMPARE_WITH_TIMEOUT(test.details, 1, 2000);
        const auto held = test.openingReply(detail);
        QVERIFY(held);
        test.resolved = second;
        controller.openTopic(openingCanonicalId, openingStreamId);
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), openingCanonicalId, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(test.lookupTargets, QStringList({"/api/workspace/v1/messenger/stream_topics/" + openingRequestedId,
            secondPath}));
        QCOMPARE(test.details, detail ? 1 : 0);
        QCOMPARE(test.targetTopicLists, 1);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(controller.error().isEmpty());
    }

    void topicOpeningLifetimeLossCancelsEachPhysicalStage_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("detail");
        for (const auto *change : {"logout", "token", "cache-replaced", "cache-destroyed"})
            for (bool detail : {false, true})
                QTest::newRow(qPrintable(QString(change) + (detail ? "-detail" : "-topic")))
                    << QString(change) << detail;
    }
    void topicOpeningLifetimeLossCancelsEachPhysicalStage()
    {
        QFETCH(QString, change);
        QFETCH(bool, detail);
        QTemporaryDir firstDirectory, secondDirectory;
        QVERIFY(firstDirectory.isValid() && secondDirectory.isValid());
        TopicOpeningFixture test;
        std::unique_ptr<NativeCacheService> cache, replacement;
        if (change.startsWith("cache-")) {
            cache = std::make_unique<NativeCacheService>(firstDirectory.path(), NativeCache::Policy{}, true);
            test.fixture.controller.useCache(cache.get());
        }
        if (detail) test.detailDelay = 30000; else test.lookupDelay = 30000;
        QVERIFY(test.start());
        if (cache) { QTRY_VERIFY_WITH_TIMEOUT(cache->available(), 2000); QVERIFY(flushCache(*cache)); }
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        if (detail) QTRY_COMPARE_WITH_TIMEOUT(test.details, 1, 2000);
        const auto held = test.openingReply(detail);
        QVERIFY(held);
        QVERIFY(controller.topicResolving());
        if (change == "logout") controller.logout();
        else if (change == "token") {
            const auto generation = test.fixture.api.generation();
            test.fixture.api.setAccessToken("synthetic-replaced-opening-token");
            QVERIFY(test.fixture.api.generation() > generation);
        } else if (change == "cache-replaced") {
            replacement = std::make_unique<NativeCacheService>(secondDirectory.path(), NativeCache::Policy{}, true);
            controller.useCache(replacement.get());
        } else cache.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QVERIFY(!controller.topicResolving());
        QCOMPARE(controller.selectedStream(), change == "logout" ? QString{} : streamId);
        QCOMPARE(controller.selectedTopic(), change == "logout" ? QString{} : topicId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, detail ? 1 : 0);
        QCOMPARE(test.mutations, 0);
        // Replacement/destroy recovery may refresh the current catalog. It must
        // never commit the delayed target, regardless of optional recovery GETs.
        for (const auto &target : test.historyTargets)
            QVERIFY(QUrlQuery(target).queryItemValue("stream_uuid") != openingStreamId);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void topicOpeningPublicationHonoursReentrantSelection_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("admission-to-current-known") << QString("admission");
        QTest::newRow("detail-to-new-unknown") << QString("detail");
        QTest::newRow("route-to-origin-stream") << QString("route");
    }
    void topicOpeningPublicationHonoursReentrantSelection()
    {
        QFETCH(QString, phase);
        TopicOpeningFixture test;
        const auto secondPath = "/api/workspace/v1/messenger/stream_topics/" + openingCanonicalId;
        const auto second = creationTopic(openingCanonicalId, openingStreamId, "Reentrant canonical topic");
        test.additionalLookups.insert(secondPath, Response{second});
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        bool replaced = false;
        QObject observer;
        const auto replace = [&] {
            if (replaced) return;
            replaced = true;
            if (phase == "admission") controller.openTopic(topicId);
            else if (phase == "detail") {
                test.resolved = second;
                controller.openTopic(openingCanonicalId);
            } else controller.selectStream(streamId);
        };
        if (phase == "detail") connect(&controller, &NativeController::streamsChanged, &observer, replace);
        else connect(&controller, &NativeController::stateChanged, &observer, [&] {
            if ((phase == "admission" && controller.topicResolving() && controller.selectedStream() == streamId)
                || (phase == "route" && controller.selectedStream() == openingStreamId)) replace();
        });
        controller.openTopic(openingRequestedId);
        QTRY_VERIFY_WITH_TIMEOUT(replaced, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving() && !controller.busy(), 2000);
        QCOMPARE(controller.selectedStream(), phase == "detail" ? openingStreamId : streamId);
        QCOMPARE(controller.selectedTopic(), phase == "detail" ? openingCanonicalId : phase == "admission" ? topicId : QString{});
        QCOMPARE(test.lookups, phase == "admission" ? 0 : phase == "detail" ? 2 : 1);
        QCOMPARE(test.details, phase == "admission" ? 0 : 1);
        QCOMPARE(test.mutations + test.catalogs + test.users, 0);
        for (const auto &target : test.historyTargets)
            QVERIFY(QUrlQuery(target).queryItemValue("topic_uuid") != openingRequestedId);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(controller.error().isEmpty());
    }

    void topicOpeningPublicationMayDestroyItsFixture_data()
    {
        QTest::addColumn<bool>("afterDetail");
        QTest::newRow("admission") << false;
        QTest::newRow("owned-stream-detail") << true;
    }
    void topicOpeningPublicationMayDestroyItsFixture()
    {
        QFETCH(bool, afterDetail);
        auto test = std::make_unique<TopicOpeningFixture>();
        QVERIFY(test->start());
        QPointer<NativeController> controller = &test->fixture.controller;
        int lookups = -1, details = -1, histories = -1;
        QObject observer;
        const auto destroy = [&] {
            lookups = test->lookups;
            details = test->details;
            histories = test->aggregateHistory + test->topicHistory;
            test.reset();
        };
        if (afterDetail) connect(controller, &NativeController::streamsChanged, &observer, destroy);
        else connect(controller, &NativeController::stateChanged, &observer, [&] {
            if (controller && controller->topicResolving()) destroy();
        });
        controller->openTopic(openingRequestedId);
        QTRY_VERIFY_WITH_TIMEOUT(controller.isNull(), 2000);
        QVERIFY(!test);
        QCOMPARE(lookups, afterDetail ? 1 : 0);
        QCOMPARE(details, afterDetail ? 1 : 0);
        QCOMPARE(histories, 0);
    }

    void seededTopicMustUseTheCompleteListAndRecoverAnExplicitRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), NativeCache::Policy{}, true);
        TopicOpeningFixture test;
        test.fixture.controller.useCache(&cache);
        QVERIFY(test.start());
        QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 2000);
        QVERIFY(flushCache(cache));
        const auto old = creationTopic(secondTopicId, openingStreamId, "Older cached sibling");
        QVERIFY(cache.put(cache.lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}}, QJsonArray{old}));
        const auto page = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}});
        QVERIFY(page && page->fresh);
        test.topicsDelay = 30000;
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), openingRequestedId, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(test.targetTopicLists, 1, 2000);
        QVERIFY(!controller.topicsComplete());
        QCOMPARE(controller.topics().size(), 1);
        QCOMPARE(controller.topics().first().toMap().value("id").toString(), openingRequestedId);
        controller.setDraft("Only the complete owned list may authorize sending");
        QVERIFY(!controller.canSend());
        test.topicsDelay = 0;
        controller.selectTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.targetTopicLists, 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.topicsComplete() && !controller.busy(), 2000);
        QCOMPARE(controller.selectedTopic(), openingRequestedId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 1);
        QCOMPARE(test.users + test.catalogs + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(flushCache(cache));
        const auto authoritative = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}});
        QVERIFY(authoritative && authoritative->fresh);
        QCOMPARE(authoritative->rows.size(), 1);
        QCOMPARE(authoritative->rows.first().toObject().value("uuid").toString(), openingRequestedId);
    }

    void completeTopicListMayRemoveTheCanonicalSeedWithoutInventingMembership()
    {
        TopicOpeningFixture test;
        test.omitResolvedFromList = true;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.targetTopicLists, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.topicsComplete() && !controller.busy(), 2000);
        QCOMPARE(controller.selectedStream(), openingStreamId);
        QVERIFY(controller.selectedTopic().isEmpty());
        QVERIFY(controller.topics().isEmpty());
        QVERIFY(!controller.canSend());
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 1);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void pendingTopicOpeningDistinguishesMetadataFromSecurityEvents_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"name-and-counter", "role", "topic-deleted"})
            QTest::newRow(change) << QString(change);
    }
    void pendingTopicOpeningDistinguishesMetadataFromSecurityEvents()
    {
        QFETCH(QString, change);
        TopicOpeningFixture test;
        test.lookupDelay = 30000;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        const auto held = test.openingReply();
        QVERIFY(held);
        if (change == "topic-deleted") {
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted",
                QJsonObject{{"uuid", openingRequestedId}, {"stream_uuid", openingStreamId}}, 80)));
        } else {
            auto row = creationChannel(streamId, "Renamed channel");
            row["unread_count"] = 8;
            row["active_unread_count"] = 8;
            if (change == "role") row["role"] = "member";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row, 80)));
        }
        if (change == "name-and-counter") {
            QVERIFY(controller.topicResolving());
            QVERIFY(held);
            controller.openTopic(topicId);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QVERIFY(!controller.topicResolving());
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void unknownLivePlacementRespectsLoadedOlderBoundary_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("empty"); QTest::addColumn<bool>("older");
        for(const auto *kind:{"message.created","message.updated"}) {
            QTest::newRow(qPrintable(QString(kind)+"-older"))<<QString(kind)<<false<<true;
            QTest::newRow(qPrintable(QString(kind)+"-newer"))<<QString(kind)<<false<<false;
            QTest::newRow(qPrintable(QString(kind)+"-empty"))<<QString(kind)<<true<<true;
        }
    }
    void unknownLivePlacementRespectsLoadedOlderBoundary() {
        QFETCH(QString,kind); QFETCH(bool,empty); QFETCH(bool,older); Fixture fixture;
        QMap<int,QJsonObject> rows; for(int n=1;n<=70;++n) rows.insert(n,historyMessage(n));
        fixture.messages=[&](const Request &request)->Response {
            if(empty) return {QJsonArray{},200,0,{{"X-Pagination-Marker","valid-older-cursor"}}};
            return historyResponse(rows,request);
        };
        QVERIFY(fixture.start()); auto &c=fixture.controller; QVERIFY(c.canLoadOlder());
        const auto before=c.messages().size(); const auto gets=fixture.totalHistoryGets();
        auto incoming=historyMessage(older?0:100);
        QVERIFY(c.applyRealtimeEvent(controllerEvent(kind,incoming)));
        const bool insert=!older&&!empty;
        QCOMPARE(!modelMessage(c,incoming.value("uuid").toString()).isEmpty(),insert);
        QCOMPARE(c.messages().size(),before+(insert?1:0)); QCOMPARE(fixture.totalHistoryGets(),gets);
        QVERIFY(c.canLoadOlder());
    }
    void livePlacementLeavesTheFormerSelectedConversation_data() {
        QTest::addColumn<bool>("crossStream"); QTest::addColumn<bool>("aggregate");
        QTest::newRow("topic-same-stream") << false << false;
        QTest::newRow("topic-cross-stream") << true << false;
        QTest::newRow("aggregate-same-stream-control") << false << true;
        QTest::newRow("aggregate-cross-stream") << true << true;
    }
    void livePlacementLeavesTheFormerSelectedConversation() {
        QFETCH(bool,crossStream); QFETCH(bool,aggregate); Fixture fixture;
        auto row=message("Same identity after a placement move"); fixture.aggregateMessages=true;
        fixture.messages=[&](const Request &request) -> Response {
            const auto query=QUrlQuery(request.target); const auto stream=query.queryItemValue("stream_uuid"),topic=query.queryItemValue("topic_uuid");
            return {row.value("stream_uuid").toString()==stream && (topic.isEmpty() || row.value("topic_uuid").toString()==topic)?QJsonArray{row}:QJsonArray{}};
        };
        QVERIFY(fixture.start()); auto &c=fixture.controller;
        if(aggregate) { c.selectStream(streamId); QTRY_VERIFY(!c.busy()); }
        QCOMPARE(c.messages().size(),1); const auto selectedTopic=c.selectedTopic();
        row["topic_uuid"]=secondTopicId; if(crossStream)row["stream_uuid"]=otherArchiveId;
        const auto gets=fixture.totalHistoryGets();
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.updated",row)));
        QCOMPARE(c.selectedStream(),streamId); QCOMPARE(c.selectedTopic(),selectedTopic);
        if(aggregate && !crossStream) {
            QCOMPARE(c.messages().size(),1); QCOMPARE(modelMessage(c,messageId).value("topicId").toString(),secondTopicId);
        } else {
            QVERIFY2(modelMessage(c,messageId).isEmpty(),"A moved message still belongs to the former selected conversation");
        }
        QCOMPARE(fixture.totalHistoryGets(),gets);
    }
    void liveMoveRetainsIndexedHistoryBodyOutsideSavedDestination_data()
    {
        QTest::addColumn<bool>("destinationWindow");
        QTest::newRow("no-destination-window") << false;
        QTest::newRow("outside-older-bound") << true;
    }

    void liveMoveRetainsIndexedHistoryBodyOutsideSavedDestination()
    {
        QFETCH(bool, destinationWindow);
        auto served = message("Known history body"); served["user_uuid"] = userId;
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [served](const Request &) { return Response{QJsonArray{served}}; };
        QVERIFY(fixture.start()); QTRY_VERIFY(cache.available()); QVERIFY(flushCache(cache));
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        const auto lease = cache.lease();
        const NativeCache::Key source{NativeCache::Kind::History, streamId, topicId, {}};
        const NativeCache::Key destination{NativeCache::Kind::History, otherArchiveId, secondTopicId, {}};
        const NativeCache::Key destinationAggregate{NativeCache::Kind::History, otherArchiveId, {}, {}};
        const NativeCache::Key bodyKey{NativeCache::Kind::MessageBody, {}, {}, messageId.toUtf8()};
        const auto savedSource = cacheEntry(cache, lease, source); QVERIFY(savedSource);
        QCOMPARE(savedSource->rows, QJsonArray{served});
        QVERIFY(!cacheEntry(cache, lease, bodyKey)); // The normal History response did not seed a standalone Body.
        const auto readBody = [&](std::optional<NativeCache::MessageBody> &result) {
            struct Read { bool done = false; std::optional<NativeCache::MessageBody> value; };
            const auto state = std::make_shared<Read>();
            cache.getMessageBody(lease, messageId, &cache, [state](auto value) { state->value = std::move(value); state->done = true; });
            const bool done = QTest::qWaitFor([state] { return state->done; }, 2000);
            result = state->value; return done;
        };
        std::optional<NativeCache::MessageBody> before;
        QVERIFY(readBody(before)); QVERIFY(before); QCOMPARE(before->row, served);
        auto latest = message("Destination newer row", returnedId, secondTopicId);
        latest["stream_uuid"] = otherArchiveId; latest["user_uuid"] = userId;
        latest["created_at"] = "2026-01-02T10:01:00.000Z";
        if (destinationWindow) { QVERIFY(cache.put(lease, destination, {latest}, "destination-older")); QVERIFY(flushCache(cache)); }
        auto moved = served; moved["stream_uuid"] = otherArchiveId; moved["topic_uuid"] = secondTopicId;
        moved["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Canonical moved body"}};
        const auto gets = fixture.totalHistoryGets();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", moved)));
        QCOMPARE(fixture.controller.selectedStream(), streamId); QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(modelMessage(fixture.controller, messageId).isEmpty());
        QCOMPARE(fixture.totalHistoryGets(), gets); QVERIFY(flushCache(cache));
        const auto previousPage = cacheEntry(cache, lease, source); QVERIFY(previousPage); QVERIFY(previousPage->rows.isEmpty());
        const auto target = cacheEntry(cache, lease, destination); QCOMPARE(target.has_value(), destinationWindow);
        if (target) { QCOMPARE(target->rows, QJsonArray{latest}); QCOMPARE(target->nextMarker, QByteArray("destination-older")); }
        QVERIFY(!cacheEntry(cache, lease, destinationAggregate));
        std::optional<NativeCache::MessageBody> retained;
        QVERIFY(readBody(retained));
        QVERIFY2(retained.has_value(), "The real Controller move must retain the owned body without loading a destination page");
        QCOMPARE(retained->row, moved);
    }

    void wholeTopicMoveFollowsCanonicalAndEvictsVacatedBucket()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), NativeCache::Policy{}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        int vacatedGets = 0, movedGets = 0;
        fixture.streams = [](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false}},
                QJsonObject{{"uuid", otherArchiveId}, {"project_id", projectId}, {"name", "Receiving channel"}, {"encryption", false}}}};
        };
        fixture.topics = [](const Request &request) {
            const auto movedSide = request.target.toString().contains(otherArchiveId);
            return Response{QJsonArray{creationTopic(topicId, movedSide ? otherArchiveId : streamId,
                movedSide ? "Moved topic" : "Native topic")}};
        };
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            const auto stream = query.queryItemValue("stream_uuid");
            if (stream == streamId) ++vacatedGets; else ++movedGets;
            auto a = historyMessage(1); auto b = historyMessage(2);
            if (!stream.isEmpty()) { a["stream_uuid"] = stream; b["stream_uuid"] = stream; }
            return Response{QJsonArray{a, b}};
        };
        // Manual start: the fixture's default flow expects a single-stream catalog.
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&]{return fixture.controller.projects().size()==1;},2000));
        fixture.controller.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&]{return fixture.controller.streams().size()==2;},2000));
        fixture.controller.selectStream(streamId);
        QVERIFY(QTest::qWaitFor([&]{return !fixture.controller.topics().isEmpty();},2000));
        fixture.controller.selectTopic(topicId);
        QVERIFY(QTest::qWaitFor([&]{return !fixture.controller.busy();},2000));
        QTRY_VERIFY(cache.available());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        const auto lease = cache.lease();
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, streamId, topicId, {}}, {historyMessage(1)}, "vacated-older"));
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, streamId, {}, {}}, {historyMessage(1), historyMessage(2)}));
        auto foreign = historyMessage(9);
        foreign["stream_uuid"] = otherArchiveId; foreign["topic_uuid"] = secondTopicId;
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, otherArchiveId, secondTopicId, {}}, {foreign}));
        QVERIFY(flushCache(cache));
        QVERIFY(cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, topicId, {}}));
        QVERIFY(cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, {}, {}}));
        QVERIFY2(cacheEntry(cache, lease, {NativeCache::Kind::History, otherArchiveId, secondTopicId, {}}), "pre: unrelated page must be seeded");
        const auto vacatedBaseline = vacatedGets;
        const auto movedTopic = creationTopic(topicId, otherArchiveId, "Moved topic");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", movedTopic)));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.selectedStream(), otherArchiveId, 4000);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(flushCache(cache));
        QVERIFY2(!cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, topicId, {}}),
            "A whole-topic move is authoritative; the vacated topic bucket must not hydrate the former route");
        QVERIFY2(!cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, {}, {}}),
            "The vacated stream aggregate bucket must not hydrate messages that moved to another channel");
        QVERIFY(cacheEntry(cache, lease, {NativeCache::Kind::History, otherArchiveId, secondTopicId, {}}));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.controller.messages().size(), 2);
        const auto firstId = historyMessage(1).value("uuid").toString();
        const auto secondId = historyMessage(2).value("uuid").toString();
        QCOMPARE(modelMessage(fixture.controller, firstId).size() > 0, true);
        QCOMPARE(modelMessage(fixture.controller, secondId).size() > 0, true);
        QCOMPARE(vacatedGets, vacatedBaseline);
        const auto stormGets = fixture.totalHistoryGets();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", creationTopic(topicId, otherArchiveId, "Storm one"), 21)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", creationTopic(topicId, otherArchiveId, "Storm two"), 22)));
        QCOMPARE(fixture.controller.selectedStream(), otherArchiveId);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QCOMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.totalHistoryGets(), stormGets);
        int movedTopicRows = 0;
        for (const auto &row : fixture.controller.topics())
            if (row.toMap().value("id").toString() == topicId) ++movedTopicRows;
        QCOMPARE(movedTopicRows, 1);
    }

    void delayedOldHistoryCannotResurrectMovedPlacement() {
        Fixture fixture; auto row=message("Current placement"); int reads=0;
        fixture.messages=[&](const Request &request)->Response {
            ++reads; const auto topic=QUrlQuery(request.target).queryItemValue("topic_uuid");
            return {topic==row.value("topic_uuid").toString()?QJsonArray{row}:QJsonArray{},200,reads==2?150:0,{}};
        };
        QVERIFY(fixture.start()); auto &c=fixture.controller; QCOMPARE(c.messages().size(),1);
        c.refresh(); QTRY_VERIFY(reads>=2);
        row["topic_uuid"]=secondTopicId;
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.updated",row)));
        QTRY_VERIFY(!c.busy());
        QVERIFY2(modelMessage(c,messageId).isEmpty(),"An older in-flight REST page restored moved membership");
    }

    static QString streamDeepLink(const Fixture &fixture, const QString &tail)
    {
        return "ew://open/org/127.0.0.1-" + QString::number(fixture.server.origin().port())
            + "/project/" + projectId + "/stream/" + streamId + tail;
    }

    void deepLinkStreamAndTopicRoutes_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("segment");
        QTest::addColumn<bool>("defaultTopic");
        QTest::newRow("name") << QString("Second topic") << QString("Second%20topic") << false;
        QTest::newRow("uuid") << QString("Second topic") << secondTopicId << false;
        QTest::newRow("unicode-reserved") << QString::fromUtf8("План #1? + 50%")
            << QString::fromUtf8(QUrl::toPercentEncoding(QString::fromUtf8("План #1? + 50%"))) << false;
        QTest::newRow("decode-once") << QString("Percent%20name") << QString("Percent%2520name") << false;
        QTest::newRow("default-token") << QString("General") << QString("__empty__") << true;
        QTest::newRow("default-alias") << QString("General") << QString("General%20chat") << true;
        QTest::newRow("literal-empty-token") << QString("__empty__") << QString("~__empty__") << false;
    }

    void deepLinkStreamAndTopicRoutes()
    {
        QFETCH(QString, name); QFETCH(QString, segment); QFETCH(bool, defaultTopic);
        Fixture f; f.aggregateMessages = true;
        f.topics = [&](const Request &) {
            auto target = creationTopic(secondTopicId, streamId, name);
            target["is_default"] = defaultTopic;
            return Response{QJsonArray{creationTopic(topicId, streamId, "Native topic"), target}};
        };
        f.messages = [](const Request &request) {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message(topic.isEmpty() ? "Aggregate" : topic, messageId,
                                               topic.isEmpty() ? topicId : topic)}};
        };
        QVERIFY(f.start());
        f.controller.openMessageLink(streamDeepLink(f, "/topic/" + segment + "?view=chat#latest"));
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.selectedTopic(), secondTopicId, 2500);
        QCOMPARE(f.controller.selectedStream(), streamId);
        QTRY_COMPARE(modelMessage(f.controller, messageId).value("content").toString(), secondTopicId);
        f.controller.openMessageLink(streamDeepLink(f, ""));
        QTRY_VERIFY(f.controller.selectedTopic().isEmpty());
        QTRY_COMPARE(modelMessage(f.controller, messageId).value("content").toString(), QString("Aggregate"));
    }

    void deepLinkStreamRouteRejectsForeignAndUnknownTargets_data()
    {
        QTest::addColumn<QString>("target");
        for (const auto *name : {"organization", "project", "stream", "topic", "ambiguous"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void deepLinkStreamRouteRejectsForeignAndUnknownTargets()
    {
        QFETCH(QString, target);
        Fixture f;
        if (target == "ambiguous") f.topics = [](const Request &) {
            return Response{QJsonArray{creationTopic(topicId, streamId, "Missing"),
                                       creationTopic(secondTopicId, streamId, "Missing")}};
        };
        QVERIFY(f.start());
        QString url = streamDeepLink(f, "/topic/Missing");
        if (target == "organization") url.replace("/org/127.0.0.1-", "/org/foreign-");
        else if (target == "project") url.replace(projectId, secondTopicId);
        else if (target == "stream") url.replace(streamId, secondTopicId);
        QVERIFY(f.controller.notice().isEmpty());
        f.controller.openMessageLink(url);
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.notice().isEmpty(), 2000);
        QCOMPARE(f.controller.selectedStream(), streamId);
        QCOMPARE(f.controller.selectedTopic(), (target == "topic" || target == "ambiguous") ? QString{} : topicId);
    }

    void deepLinkTopicBeforeLoginKeepsTheLatestRoute()
    {
        Fixture f; f.extraTopic = true;
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Native%20topic"));
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Second%20topic"));
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.selectedTopic(), secondTopicId, 3500);
    }

    void deepLinkNewReadyRouteReplacesAnOlderQueuedRoute()
    {
        Fixture f; f.extraTopic = true;
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Native%20topic"));
        QVERIFY(f.start());
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Second%20topic"));
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.selectedTopic(), secondTopicId, 2000);
        QTest::qWait(1000);
        QCOMPARE(f.controller.selectedTopic(), secondTopicId);
    }

    void deepLinkSelectsTheNamedTopicInAnotherStream()
    {
        Fixture f;
        const QString destination = "8f8f8f8f-8f8f-4f8f-8f8f-8f8f8f8f8f8f";
        f.topics = [&](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            return Response{QJsonArray{creationTopic(stream == destination ? secondTopicId : topicId,
                                                      stream, "Native topic")}};
        };
        QVERIFY(f.start());
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream.created", creationChannel(destination))));
        QTRY_COMPARE(f.controller.streams().size(), 2);
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Native%20topic").replace(streamId, destination));
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.selectedStream(), destination, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.selectedTopic(), secondTopicId, 2000);
    }

    void deepLinkTopicWaitsForAnAggregateToFinishLoading()
    {
        Fixture f; f.extraTopic = true; f.aggregateMessages = true;
        bool delayed = false;
        f.messages = [&](const Request &) { return Response{QJsonArray{}, 200, delayed ? 300 : 0}; };
        QVERIFY(f.start()); delayed = true;
        f.controller.selectStream(streamId);
        QVERIFY(f.controller.busy());
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Second%20topic"));
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.selectedTopic(), secondTopicId, 3500);
    }

    void deepLinkTopicListReentrantNavigationWins()
    {
        Fixture f; f.extraTopic = true; QVERIFY(f.start());
        bool replaced = false;
        connect(&f.controller, &NativeController::topicsChanged, &f.controller, [&] {
            if (!replaced && f.controller.topics().size() == 2) {
                replaced = true;
                f.controller.showInbox();
            }
        });
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Second%20topic"));
        QTRY_VERIFY(replaced);
        QTest::qWait(100);
        QVERIFY(f.controller.inboxVisible());
        QVERIFY(f.controller.selectedTopic().isEmpty());
    }

    void deepLinkPendingTopicIsSupersededAndRetired_data()
    {
        QTest::addColumn<QString>("replacement");
        for (const auto *name : {"link", "selection", "logout", "generation", "inbox", "inbox-roundtrip"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void deepLinkPendingTopicIsSupersededAndRetired()
    {
        QFETCH(QString, replacement);
        Fixture f; int gets = 0; bool delay = false;
        f.topics = [&](const Request &) {
            ++gets;
            return Response{QJsonArray{creationTopic(topicId, streamId, "Native topic"),
                                       creationTopic(secondTopicId, streamId, "Second topic")}, 200, delay ? 600 : 0};
        };
        QVERIFY(f.start()); delay = true;
        const int initial = gets;
        f.controller.openMessageLink(streamDeepLink(f, "/topic/Second%20topic"));
        QTRY_VERIFY_WITH_TIMEOUT(gets > initial, 2000);
        if (replacement == "link") f.controller.openMessageLink(streamDeepLink(f, "/topic/Native%20topic"));
        else if (replacement == "selection") f.controller.selectStream(streamId);
        else if (replacement == "logout") f.controller.logout();
        else if (replacement == "generation") f.api.setAccessToken("synthetic-replacement-token");
        else if (replacement == "inbox-roundtrip") { f.controller.showInbox(); f.controller.closeInbox(); }
        else f.controller.openMessageLink(streamDeepLink(f, "").section("/stream/", 0, 0) + "/inbox");
        QTest::qWait(1000);
        if (replacement == "link") QCOMPARE(f.controller.selectedTopic(), topicId);
        else QVERIFY(f.controller.selectedTopic() != secondTopicId);
    }

    void deepLinkCopyProducesTheCanonicalUrl()
    {
        Fixture fixture;
        auto row = message("Link me");
        fixture.messages = [row](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        const QString expected = origin.toString() + "/org/127.0.0.1-"
            + QString::number(origin.port()) + "/project/" + projectId + "/message/" + messageId;
        QCOMPARE(fixture.controller.messageLink(messageId), expected);
        fixture.controller.copyMessageLink(messageId);
        QCOMPARE(QGuiApplication::clipboard()->text(), expected);
        QVERIFY(fixture.controller.messageLink("ffffffff-ffff-4fff-8fff-ffffffffffff").isEmpty());
    }

    void deepLinkOpensTargetTopicAndAnchorsTheMessage()
    {
        Fixture fixture; fixture.extraTopic = true; fixture.aggregateMessages = true;
        const QString target = "77777777-7777-4777-8777-777777777777";
        const auto origin = fixture.server.origin();
        int lookups = 0;
        QString lookupProject, lookupUuid;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) {
                lookupProject = query.queryItemValue("project_id");
                lookupUuid = query.queryItemValue("uuid");
                ++lookups;
                return Response{QJsonArray{message("Deep body", target, secondTopicId)}};
            }
            if (request.method == "GET" && query.queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{message("Deep body", target, secondTopicId)}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QSignalSpy anchors(&fixture.controller, &NativeController::anchorChanged);
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + target);
        QTRY_COMPARE(fixture.controller.selectedTopic(), secondTopicId);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), target);
        QVERIFY(anchors.count() >= 1);
        QCOMPARE(lookups, 1);
        QCOMPARE(lookupProject, projectId);
        QCOMPARE(lookupUuid, target);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.anchorMessageId().isEmpty(), 4500);
    }

    void deepLinkOpensUncataloguedTopicAndAnchorsTheMessage()
    {
        // Issue #55: the target topic lives in another channel and is absent
        // from the currently loaded topic catalog, so openTopic() runs
        // beginTopicOpening(). Its intermediate pre-commit stateChanged
        // emissions must not cancel the pending anchor, and the bounded
        // older-history search must still reach the message.
        Fixture fixture; fixture.aggregateMessages = true;
        const QString otherStream = "39393939-3939-4939-8939-393939393939";
        const QString ghostTopic = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        const QString target = "7c7c7c7c-7c7c-4c7c-8c7c-7c7c7c7c7c7c";
        // ghostTopic (and its channel) are absent from the loaded catalogs;
        // both are resolved through their detail GETs while the deep link runs.
        fixture.topics = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == otherStream)
                return Response{QJsonArray{QJsonObject{{"uuid", ghostTopic}, {"project_id", projectId},
                    {"stream_uuid", otherStream}, {"name", "Ghost topic"}}}};
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        auto targetRow = message("Deep uncatalogued body", target, ghostTopic);
        targetRow.insert("stream_uuid", otherStream);
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/streams/" + otherStream)
                return Response{creationChannel(otherStream, "Second channel")};
            // Delayed topic metadata: the canonical GET lands after beginTopicOpening()
            // has already emitted its own stateChanged signals.
            if (request.target.path() == "/api/workspace/v1/messenger/stream_topics/" + ghostTopic)
                return Response{creationTopic(ghostTopic, otherStream, "Ghost topic"), 200, 250, {}};
            return fallback(request);
        };
        int olderPages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) return Response{QJsonArray{targetRow}};
            if (query.queryItemValue("topic_uuid") == ghostTopic) {
                if (query.hasQueryItem("page_marker")) {
                    ++olderPages;
                    return Response{QJsonArray{targetRow}, 200, 0, {}};
                }
                auto filler = message("Newer ghost row", "7d7d7d7d-7d7d-4d7d-8d7d-7d7d7d7d7d7d", ghostTopic);
                filler.insert("stream_uuid", otherStream);
                return Response{QJsonArray{filler}, 200, 0, {{"X-Pagination-Marker", "older-1"}}};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QSignalSpy anchors(&fixture.controller, &NativeController::anchorChanged);
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + target);
        QTRY_COMPARE(fixture.controller.selectedStream(), otherStream);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.selectedTopic(), ghostTopic, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.anchorMessageId(), target, 5000);
        QVERIFY(anchors.count() >= 1);
        QVERIFY(olderPages >= 1); // The target was only on an older page.
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.anchorMessageId().isEmpty(), 4500);
    }

    void deepLinkForeignOrganizationOrProjectKeepsSelection()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const auto origin = fixture.server.origin();
        QVERIFY(fixture.start());
        const auto beforeStream = fixture.controller.selectedStream();
        const auto beforeTopic = fixture.controller.selectedTopic();
        fixture.controller.openMessageLink("ew://open/org/somewhere-else/project/" + projectId
            + "/message/" + messageId);
        QTest::qWait(200);
        QCOMPARE(fixture.controller.selectedStream(), beforeStream);
        QCOMPARE(fixture.controller.selectedTopic(), beforeTopic);
        QVERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/9e9e9e9e-9e9e-4e9e-8e9e-9e9e9e9e9e9e/message/" + messageId);
        QTest::qWait(200);
        QCOMPARE(fixture.controller.selectedStream(), beforeStream);
        QCOMPARE(fixture.controller.selectedTopic(), beforeTopic);
        QVERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
    }

    void deepLinkMissingMessageLandsSafely()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        fixture.messages = [](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) return Response{QJsonArray{}};
            if (request.method == "GET" && query.hasQueryItem("topic_uuid"))
                return Response{QJsonArray{message("Current body")}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/7a7a7a7a-7a7a-4a7a-8a7a-7a7a7a7a7a7a");
        QTest::qWait(300);
        QCOMPARE(fixture.controller.selectedStream(), streamId);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
    }

    void deepLinkBeforeLoginAppliesWhenTheSessionSettles()
    {
        Fixture fixture; fixture.extraTopic = true; fixture.aggregateMessages = true;
        const QString target = "78787878-7878-4787-8787-787878787878";
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{message("Cold body", target, secondTopicId)}};
            if (request.method == "GET" && query.queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{message("Cold body", target, secondTopicId)}};
            return Response{QJsonArray{}};
        };
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + target);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.selectedTopic(), secondTopicId);
        QTRY_VERIFY(fixture.controller.anchorMessageId() == target
            || fixture.controller.anchorMessageId().isEmpty()); // highlight may already have cleared
    }
};

QTEST_MAIN(ControllerNavigationTest)
#include "native-controller-navigation-test.moc"
