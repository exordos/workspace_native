#include "mail-store.h"
#include "support/native-test-harness.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-mention-fixtures.h"
#include "support/controller-folder-fixtures.h"

class ControllerInboxTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Inbox page (issue #44, docs/NATIVE_DESKTOP_INBOX.md) ----

    // Stream/topic fixtures measured against workspace_ui
    // 61dacef78dab2f5d0be76f283bebf49fe29b84af inbox-page.ui.tsx: DM + channel
    // streams mixing active/passive counters, a passive-only stream that must
    // stay out of the inbox, and a stream whose only unread lives at stream
    // level (reference fallback-row rule).
    static QJsonObject inboxStream(const QString &id, const QString &name,
        qint64 unread, qint64 active, qint64 passive, bool isPrivate = false,
        const QString &directUser = QString(),
        const QString &updatedAt = QStringLiteral("2026-06-22T10:10:00Z"))
    {
        return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"owner", userId},
            {"name", name}, {"description", ""}, {"role", "owner"}, {"notification_mode", "all_messages"},
            {"source_name", "native"}, {"provider_uuid", QJsonValue::Null}, {"provider_identity", QJsonValue::Null},
            {"invite_only", false}, {"announce", false}, {"private", isPrivate}, {"is_archived", false},
            {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null},
            {"direct_user_uuid", directUser.isEmpty() ? QJsonValue::Null : QJsonValue(directUser)},
            {"default_topic_uuid", QJsonValue::Null}, {"unread_count", unread},
            {"active_unread_count", active}, {"passive_unread_count", passive},
            {"last_message_uuid", QJsonValue::Null}, {"updated_at", updatedAt}};
    }

    static QJsonObject inboxTopic(const QString &id, const QString &stream, const QString &name,
        qint64 unread, qint64 active, qint64 passive,
        const QString &updatedAt = QStringLiteral("2026-06-22T10:10:00Z"))
    {
        return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"stream_uuid", stream},
            {"name", name}, {"is_done", false}, {"is_default", false}, {"notification_mode", "default"},
            {"unread_count", unread}, {"active_unread_count", active}, {"passive_unread_count", passive},
            {"last_message_uuid", QJsonValue::Null}, {"created_at", "2026-06-22T10:00:00Z"},
            {"updated_at", updatedAt}};
    }

    void loginInbox(Fixture &fixture, int streamCount)
    {
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&fixture] { return fixture.controller.projects().size() == 1; }, 2000));
        fixture.controller.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&fixture, streamCount] {
            return fixture.controller.streams().size() == streamCount; }, 3000));
        QVERIFY(QTest::qWaitFor([&fixture] { return !fixture.controller.busy(); }, 3000));
    }

    static QVariantMap inboxCard(const QVariantMap &page, const QString &section, const QString &stream)
    {
        for (const auto &value : page.value(section).toList())
            if (value.toMap().value("id").toString() == stream) return value.toMap();
        return {};
    }

    static QVariantMap inboxRow(const QVariantMap &page, const QString &section,
        const QString &stream, const QString &topic)
    {
        for (const auto &value : inboxCard(page, section, stream).value("rows").toList())
            if (value.toMap().value("topic").toString() == topic) return value.toMap();
        return {};
    }

    void sidebarHydrationOnlyNotifiesInboxWhenVisible_data()
    {
        QTest::addColumn<bool>("inboxVisible");
        QTest::newRow("closed") << false;
        QTest::newRow("open") << true;
    }

    void sidebarHydrationOnlyNotifiesInboxWhenVisible()
    {
        QFETCH(bool, inboxVisible);
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Engineering", 1, 1, 0)}}; };
        auto held = std::make_shared<std::function<void()>>();
        fixture.topics = [held](const Request &) {
            Response response{QJsonArray{inboxTopic(topicId, streamId, "Planning", 1, 1, 0)}};
            response.defer = [held](auto send) { *held = std::move(send); };
            return response;
        };
        loginInbox(fixture, 1);
        fixture.controller.requestSidebarTopics(streamId);
        QTRY_VERIFY_WITH_TIMEOUT(bool(*held), 3000);
        if (inboxVisible) fixture.controller.showInbox();
        QSignalSpy state(&fixture.controller, &NativeController::stateChanged);
        QSignalSpy topics(&fixture.controller, &NativeController::sidebarTopicsChanged);
        (*held)(); *held = {};
        QTRY_VERIFY_WITH_TIMEOUT(!topics.isEmpty(), 3000);
        QCOMPARE(state.size(), inboxVisible ? 1 : 0);
        QCOMPARE(fixture.controller.sidebarTopicState(streamId, true).value("rows").toList().size(), 1);
        if (inboxVisible)
            QVERIFY(!inboxRow(fixture.controller.inboxPage(), "channels", streamId, topicId).isEmpty());
    }

    void sidebarExpansionLargeCatalogDiagnostic()
    {
        Fixture fixture;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("expansion-large.ini"));
        QStringList streamIds;
        QJsonArray catalog;
        for (int i = 0; i < 120; ++i) {
            const auto id = QString("93000000-0000-4000-8000-%1").arg(i + 1, 12, 10, QLatin1Char('0'));
            streamIds.append(id); catalog.append(inboxStream(id, QString("Stream %1").arg(i), 0, 0, 0));
        }
        preferences.recordExpandedStreams(conversationPreferenceOwner(fixture.server.origin()), streamIds);
        fixture.controller.usePreferences(&preferences);
        fixture.streams = [catalog](const Request &) { return Response{catalog}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto gets = std::make_shared<QHash<QString, int>>();
        fixture.topics = [gates, gets](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            ++(*gets)[stream];
            Response response{QJsonArray{inboxTopic(topicId, stream, "Planning", 0, 0, 0)}};
            response.defer = [gates](auto send) { gates->push_back(std::move(send)); };
            return response;
        };
        QElapsedTimer timer; timer.start();
        loginInbox(fixture, 120);
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 3000);
        for (const auto &id : streamIds) fixture.controller.requestSidebarTopics(id);
        int pendingReplies = 0;
        for (auto *reply : fixture.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path().endsWith("/stream_topics/")) ++pendingReplies;
        QElapsedTimer projection; projection.start();
        for (int i = 0; i < 20; ++i) QCOMPARE(fixture.controller.sidebarStreams().size(), 120);
        const auto projectionMs = projection.elapsed();
        QVERIFY2(pendingReplies <= 2, "Expansion must reserve transport capacity for the current chat and search");
        bool ticked = false; QTimer::singleShot(0, [&] { ticked = true; });
        QTRY_VERIFY_WITH_TIMEOUT(ticked, 1000);
        qInfo() << "EXPANSION_LOAD" << "streams" << streamIds.size() << "pending-replies" << pendingReplies
                << "server-requests" << gets->size() << "elapsed-ms" << timer.elapsed()
                << "projection-20-ms" << projectionMs;
        for (auto it = gets->cbegin(); it != gets->cend(); ++it) QCOMPARE(it.value(), 1);
        fixture.controller.logout();
        QTRY_VERIFY(!fixture.controller.authenticated());
        const auto pending = std::move(*gates); gates->clear();
        for (const auto &release : pending) release();
        QTest::qWait(100);
        QVERIFY(fixture.controller.sidebarStreams().isEmpty());
    }

    void sidebarExpansionQueueProgressAndForeground_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"failure", "conversation", "search"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void sidebarExpansionQueueProgressAndForeground()
    {
        QFETCH(QString, scenario);
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("expansion-queue.ini"));
        QStringList ids; QJsonArray catalog;
        for (int i = 0; i < 5; ++i) {
            const auto id = QString("94000000-0000-4000-8000-%1").arg(i + 1, 12, 10, QLatin1Char('0'));
            ids.append(id); catalog.append(inboxStream(id, QString("Stream %1").arg(i), 0, 0, 0));
        }
        preferences.recordExpandedStreams(conversationPreferenceOwner(fixture.server.origin()), ids);
        fixture.controller.usePreferences(&preferences);
        fixture.streams = [catalog](const Request &) { return Response{catalog}; };
        auto gates = std::make_shared<QHash<QString, std::function<void()>>>();
        auto gets = std::make_shared<QHash<QString, int>>();
        fixture.topics = [&, gates, gets](const Request &request) {
            const auto id = QUrlQuery(request.target).queryItemValue("stream_uuid");
            const int number = ++(*gets)[id];
            Response response{QJsonArray{inboxTopic(topicId, id, "Needle", 0, 0, 0)},
                scenario == "failure" && id == ids[0] && number == 1 ? 503 : 200};
            if (scenario == "conversation" && fixture.controller.selectedStream() == id) return response;
            if (scenario == "search" && id != ids[0] && id != ids[1]) return response;
            response.defer = [gates, id](auto send) { gates->insert(id, std::move(send)); };
            return response;
        };
        loginInbox(fixture, 5);
        QTRY_VERIFY_WITH_TIMEOUT(gates->contains(ids[0]) && gates->contains(ids[1]), 3000);
        QCOMPARE(gets->size(), 2);
        if (scenario == "failure") {
            fixture.controller.toggleSidebarStream(ids[4]); // Cancel the last queued expansion.
            auto first = gates->take(ids[0]); first();
            QTRY_VERIFY_WITH_TIMEOUT(gates->contains(ids[2]), 3000);
            fixture.controller.requestSidebarTopics(ids[0]); // Explicit retry follows the queued work.
            auto second = gates->take(ids[1]); second();
            QTRY_VERIFY_WITH_TIMEOUT(gates->contains(ids[3]), 3000);
            auto third = gates->take(ids[2]); third();
            QTRY_VERIFY_WITH_TIMEOUT(gates->contains(ids[0]) && gets->value(ids[0]) == 2, 3000);
            auto fourth = gates->take(ids[3]); fourth();
            auto retry = gates->take(ids[0]); retry();
            QTRY_VERIFY_WITH_TIMEOUT(fixture.api.findChildren<QNetworkReply *>().isEmpty(), 3000);
            QCOMPARE(gets->value(ids[4]), 0);
        } else if (scenario == "conversation") {
            fixture.controller.selectStream(ids[4]);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.selectedStream(), ids[4], 3000);
            QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.topicsComplete(), 3000);
            QCOMPARE(gets->value(ids[4]), 1); // Foreground bypasses the two held background requests.
        } else {
            fixture.controller.setSidebarSearchQuery("needle");
            QTRY_VERIFY_WITH_TIMEOUT(gets->value(ids[2]) == 1, 3000); // Search bypasses the background queue.
        }
        fixture.controller.logout();
        QTRY_VERIFY(!fixture.controller.authenticated());
        const auto held = std::move(*gates); gates->clear();
        for (auto it = held.cbegin(); it != held.cend(); ++it) it.value()();
    }

    void sidebarExpansionDoesNotCarryStateOrLateRepliesAcrossOwners()
    {
        Fixture fixture; MockServer otherServer; QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("expansion-owner.ini"));
        preferences.recordExpandedStreams(conversationPreferenceOwner(fixture.server.origin()), {streamId});
        fixture.controller.usePreferences(&preferences);
        fixture.streams = [](const Request &) { return Response{QJsonArray{inboxStream(streamId, "Engineering", 0, 0, 0)}}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        fixture.topics = [gates](const Request &) {
            Response response{QJsonArray{inboxTopic(topicId, streamId, "Old owner", 0, 0, 0)}};
            response.defer = [gates](auto send) { gates->push_back(std::move(send)); };
            return response;
        };
        otherServer.handler = fixture.server.handler;
        loginInbox(fixture, 1); QTRY_COMPARE_WITH_TIMEOUT(gates->size(), size_t(1), 3000);
        QVERIFY(fixture.controller.expandedSidebarStreams().contains(streamId));
        fixture.controller.logout(); QTRY_VERIFY(!fixture.controller.authenticated());
        fixture.controller.login(otherServer.origin().toString(), "other-owner", "synthetic-password", "");
        QTRY_COMPARE(fixture.controller.projects().size(), 1); fixture.controller.openProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.authenticated() && fixture.controller.streams().size() == 1, 3000);
        QVERIFY(!fixture.controller.expandedSidebarStreams().contains(streamId));
        (*gates)[0](); gates->clear(); QTest::qWait(200);
        QVERIFY(!fixture.controller.expandedSidebarStreams().contains(streamId));
        QVERIFY(fixture.controller.sidebarTopicState(streamId, true).value("rows").toList().isEmpty());
    }

    // Issue #859 subtask 1: a route commit only ensures the selected stream's
    // row is expanded; the other explicitly expanded (chevron) streams must
    // survive the commit and stay hydration-eligible. Only an explicit toggle
    // removes a member. #95's per-stream toggles own the expanded set.
    void sidebarRouteCommitPreservesOtherExpandedStreams()
    {
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("expansion-route.ini"));
        QStringList ids; QJsonArray catalog;
        for (int i = 0; i < 3; ++i) {
            const auto id = QString("95000000-0000-4000-8000-%1").arg(i + 1, 12, 10, QLatin1Char('0'));
            ids.append(id); catalog.append(inboxStream(id, QString("Stream %1").arg(i), 0, 0, 0));
        }
        fixture.controller.usePreferences(&preferences);
        fixture.streams = [catalog](const Request &) { return Response{catalog}; };
        const auto topicFor = [](const QString &stream) {
            return QString("96000000-0000-4000-8000-%1").arg(stream.right(12));
        };
        fixture.topics = [topicFor](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            return Response{QJsonArray{inboxTopic(topicFor(stream), stream, "Planning", 0, 0, 0)}};
        };
        loginInbox(fixture, 3);
        // Chevron-expand A and B explicitly; C stays collapsed.
        fixture.controller.toggleSidebarStream(ids[0]);
        fixture.controller.toggleSidebarStream(ids[1]);
        QTRY_VERIFY(fixture.controller.expandedSidebarStreams().contains(ids[1]));

        // Committing a route to B must not replace the expanded set: A stays.
        fixture.controller.selectStream(ids[1]);
        QTRY_VERIFY(fixture.controller.selectedStream() == ids[1] && !fixture.controller.busy());
        QVERIFY2(fixture.controller.expandedSidebarStreams().contains(ids[0]),
            "a route commit must keep the other explicitly expanded streams open");
        QVERIFY(fixture.controller.expandedSidebarStreams().contains(ids[1]));
        QVERIFY(!fixture.controller.expandedSidebarStreams().contains(ids[2]));
        QCOMPARE(fixture.controller.selectedStream(), ids[1]);

        // A topic-route commit inside B keeps A expanded as well.
        QTRY_VERIFY(fixture.controller.topicsComplete());
        fixture.controller.selectTopic(topicFor(ids[1]));
        QTRY_VERIFY(fixture.controller.selectedTopic() == topicFor(ids[1]));
        QVERIFY2(fixture.controller.expandedSidebarStreams().contains(ids[0]),
            "a topic route commit must keep the other explicitly expanded streams open");
        QVERIFY(fixture.controller.expandedSidebarStreams().contains(ids[1]));

        // An explicit collapse removes exactly its own member.
        fixture.controller.toggleSidebarStream(ids[0]);
        QVERIFY(!fixture.controller.expandedSidebarStreams().contains(ids[0]));
        QVERIFY(fixture.controller.expandedSidebarStreams().contains(ids[1]));
        QVERIFY(!fixture.controller.expandedSidebarStreams().contains(ids[2]));

        // The persisted set follows the same contract (restart restore).
        const QStringList persisted = {ids[1]};
        QCOMPARE(preferences.loadExpandedStreams(conversationPreferenceOwner(fixture.server.origin())), persisted);
        fixture.controller.logout();
    }

    void sidebarExpansionUsesLatestSelectedCatalog_data()
    {
        QTest::addColumn<bool>("holdBackground");
        QTest::newRow("completed-background") << false;
        QTest::newRow("late-background") << true;
    }

    void sidebarExpansionUsesLatestSelectedCatalog()
    {
        QFETCH(bool, holdBackground);
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("expansion-refresh.ini"));
        fixture.controller.usePreferences(&preferences);
        const QString other = "95000000-0000-4000-8000-000000000001";
        fixture.streams = [other](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Engineering", 0, 0, 0), inboxStream(other, "Support", 0, 0, 0)}}; };
        auto revision = std::make_shared<int>(1);
        auto held = std::make_shared<std::function<void()>>();
        fixture.topics = [revision, other, held, holdBackground](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == other) return Response{QJsonArray{inboxTopic("95000000-0000-4000-8000-000000000002", other, "Support", 0, 0, 0)}};
            QJsonArray rows{inboxTopic(topicId, streamId, "Planning", 0, 0, 0)};
            if (*revision >= 2) rows.append(inboxTopic(secondTopicId, streamId, "Later topic", 0, 0, 0));
            Response result{rows};
            if (holdBackground && *revision == 1)
                result.defer = [held](auto send) { *held = std::move(send); };
            return result;
        };
        loginInbox(fixture, 2);
        const auto streamRow = [&] { return fixture.controller.sidebarTopicState(streamId, true); };
        fixture.controller.requestSidebarTopics(streamId);
        if (holdBackground) QTRY_VERIFY_WITH_TIMEOUT(bool(*held), 3000);
        else QTRY_COMPARE_WITH_TIMEOUT(streamRow().value("rows").toList().size(), 1, 3000);
        *revision = 2;
        fixture.controller.selectStream(streamId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.topicsComplete() && fixture.controller.topics().size() == 2, 3000);
        fixture.controller.selectStream(other);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.topicsComplete(), 3000);
        if (holdBackground) { (*held)(); *held = {}; QTest::qWait(250); }
        fixture.controller.requestSidebarTopics(streamId);
        QTRY_COMPARE_WITH_TIMEOUT(streamRow().value("rows").toList().size(), 2, 3000);
    }

    void sidebarExpansionDeferredCacheDoesNotReleaseHeldNetwork_data()
    {
        QTest::addColumn<QString>("terminal");
        for (const auto *terminal : {"success", "failure", "logout"})
            QTest::newRow(terminal) << QString(terminal);
    }

    void sidebarExpansionDeferredCacheDoesNotReleaseHeldNetwork()
    {
        QFETCH(QString, terminal);
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        QStringList ids; QJsonArray catalog;
        for (int i = 0; i < 3; ++i) {
            const auto id = QString("97000000-0000-4000-8000-%1").arg(i + 1, 12, 10, QLatin1Char('0'));
            ids.append(id); catalog.append(inboxStream(id, QString("Stream %1").arg(i), 0, 0, 0));
        }
        const auto cachePath = directory.filePath("cache");
        {
            NativeCache initial(cachePath, NativeCache::Policy{}, true);
            QVERIFY(initial.open(fixture.server.origin(), projectId, userId));
            QVERIFY(initial.put(initial.lease(), {NativeCache::Kind::Topics, ids[0], {}, {}},
                {inboxTopic(topicId, ids[0], "Cached", 0, 0, 0)}));
        }
        QSemaphore gate; std::atomic_bool hold{false}, entered{false};
        NativeCacheService cache(cachePath, NativeCache::Policy{}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.acquire(); }
            return QDateTime::currentMSecsSinceEpoch();
        });
        QVERIFY(cache.open(fixture.server.origin(), projectId, userId));
        QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 3000);
        hold = true;
        cache.get(cache.lease(), {NativeCache::Kind::Topics, ids[0], {}, {}}, this, [](auto) {});
        const auto release = qScopeGuard([&] { gate.release(); });
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
        fixture.controller.useCache(&cache);
        fixture.streams = [catalog](const Request &) { return Response{catalog}; };
        auto foreground = std::make_shared<std::function<void()>>();
        auto held = std::make_shared<QHash<QString, std::function<void()>>>();
        auto gets = std::make_shared<QHash<QString, int>>();
        fixture.topics = [=](const Request &request) {
            const auto id = QUrlQuery(request.target).queryItemValue("stream_uuid");
            const int count = ++(*gets)[id];
            Response response{QJsonArray{inboxTopic(topicId, id, "Current", 0, 0, 0)},
                terminal == "failure" && id == ids[0] && count == 2 ? 503 : 200};
            if (id == ids[0] && count == 1) response.defer = [foreground](auto send) { *foreground = std::move(send); };
            else response.defer = [held, id](auto send) { held->insert(id, std::move(send)); };
            return response;
        };
        loginInbox(fixture, 3);
        QVERIFY(!cache.available());
        fixture.controller.selectStream(ids[0]);
        QTRY_VERIFY_WITH_TIMEOUT(bool(*foreground), 3000);
        fixture.controller.requestSidebarTopics(ids[1]);
        fixture.controller.requestSidebarTopics(ids[0]);
        fixture.controller.requestSidebarTopics(ids[2]);
        QTRY_VERIFY_WITH_TIMEOUT(held->contains(ids[0]) && held->contains(ids[1]), 3000);
        (*foreground)(); *foreground = {};
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.topicsComplete(), 3000);
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 3000);
        QTest::qWait(500);
        QCOMPARE(gets->value(ids[2]), 0);
        if (terminal != "logout") {
            auto complete = held->take(ids[0]); complete();
            QTRY_VERIFY_WITH_TIMEOUT(held->contains(ids[2]), 3000);
        }
        fixture.controller.logout();
        const auto pending = std::move(*held); held->clear();
        for (auto it = pending.cbegin(); it != pending.cend(); ++it) it.value()();
    }

    void sidebarExpansionRetriesFailedCacheRevalidation()
    {
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("cache");
        {
            NativeCache initial(path, NativeCache::Policy{}, true);
            QVERIFY(initial.open(fixture.server.origin(), projectId, userId));
            const NativeCache::Key key{NativeCache::Kind::Topics, streamId, {}, {}};
            QVERIFY(initial.put(initial.lease(), key, {inboxTopic(topicId, streamId, "Cached", 0, 0, 0)}));
            QVERIFY(initial.markStale(initial.lease(), NativeCache::Kind::Topics, streamId));
        }
        NativeCacheService cache(path, NativeCache::Policy{}, true);
        fixture.controller.useCache(&cache);
        fixture.streams = [](const Request &) { return Response{QJsonArray{inboxStream(streamId, "Engineering", 0, 0, 0)}}; };
        auto requests = std::make_shared<int>(0);
        fixture.topics = [requests](const Request &) {
            const int n = ++*requests;
            return Response{QJsonArray{inboxTopic(topicId, streamId, "Fresh", 0, 0, 0)}, n == 1 ? 503 : 200};
        };
        loginInbox(fixture, 1); QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 3000);
        fixture.controller.requestSidebarTopics(streamId);
        QTRY_VERIFY_WITH_TIMEOUT(*requests == 1 && fixture.controller.sidebarTopicState(streamId, true).value("known").toBool(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.api.findChildren<QNetworkReply *>().isEmpty(), 3000);
        QCOMPARE(fixture.controller.sidebarTopicState(streamId, true).value("rows").toList()[0].toMap().value("name").toString(), QString("Cached"));
        fixture.controller.requestSidebarTopics(streamId);
        QTRY_COMPARE_WITH_TIMEOUT(*requests, 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.sidebarTopicState(streamId, true).value("rows").toList()[0].toMap().value("name").toString(), QString("Fresh"), 3000);
    }

    void sidebarExpansionStaleCacheKeepsNetworkBounded()
    {
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        QStringList ids; QJsonArray catalog;
        for (int i = 0; i < 8; ++i) {
            const auto id = QString("96000000-0000-4000-8000-%1").arg(i + 1, 12, 10, QLatin1Char('0'));
            ids.append(id); catalog.append(inboxStream(id, QString("Stream %1").arg(i), 0, 0, 0));
        }
        {
            NativeCache initial(directory.filePath("cache"), NativeCache::Policy{}, true);
            QVERIFY(initial.open(fixture.server.origin(), projectId, userId));
            for (const auto &id : ids) {
                const NativeCache::Key key{NativeCache::Kind::Topics, id, {}, {}};
                QVERIFY(initial.put(initial.lease(), key, {inboxTopic(topicId, id, "Cached", 0, 0, 0)}));
                QVERIFY(initial.markStale(initial.lease(), NativeCache::Kind::Topics, id));
            }
            initial.close();
        }
        NativePreferences preferences(directory.filePath("expansion-cache.ini"));
        preferences.recordExpandedStreams(conversationPreferenceOwner(fixture.server.origin()), ids);
        NativeCacheService cache(directory.filePath("cache"), NativeCache::Policy{}, true);
        fixture.controller.usePreferences(&preferences); fixture.controller.useCache(&cache);
        fixture.streams = [catalog](const Request &) { return Response{catalog}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        fixture.topics = [gates](const Request &) {
            Response response{QJsonArray{}};
            response.defer = [gates](auto send) { gates->push_back(std::move(send)); };
            return response;
        };
        loginInbox(fixture, 8); QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 3000);
        QTest::qWait(700); // Let all eligible stale-cache callbacks run while network is held.
        int pending = 0;
        for (auto *reply : fixture.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path().endsWith("/stream_topics/")) ++pending;
        qInfo() << "STALE_EXPANSION_LOAD" << "held" << gates->size() << "pending" << pending;
        QVERIFY2(pending <= 2, "Stale cache delivery must not release the network request slot");
        fixture.controller.logout(); QTRY_VERIFY(!fixture.controller.authenticated());
        const auto held = std::move(*gates); gates->clear(); for (const auto &release : held) release();
    }

    void sidebarExpansionLargePopulatedCatalogDiagnostic()
    {
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("expansion-populated.ini"));
        QStringList ids; QJsonArray catalog;
        for (int i = 0; i < 120; ++i) {
            const auto id = QString("98000000-0000-4000-8000-%1").arg(i + 1, 12, 10, QLatin1Char('0'));
            ids.append(id); catalog.append(inboxStream(id, QString("Stream %1").arg(i), 0, 0, 0));
        }
        preferences.recordExpandedStreams(conversationPreferenceOwner(fixture.server.origin()), ids);
        fixture.controller.usePreferences(&preferences);
        fixture.streams = [catalog](const Request &) { return Response{catalog}; };
        auto gets = std::make_shared<int>(0);
        fixture.topics = [gets, ids](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            const auto number = ids.indexOf(stream); QJsonArray rows; ++*gets;
            for (int j = 0; j < 20; ++j) {
                const auto id = QString("99000000-0000-4000-8000-%1").arg(number * 100 + j + 1, 12, 10, QLatin1Char('0'));
                rows.append(inboxTopic(id, stream, QString("Topic %1").arg(j), 0, 0, 0));
            }
            return Response{rows};
        };
        qint64 maxProjectionMs = 0; int publications = 0;
        connect(&fixture.controller, &NativeController::sidebarStreamsChanged, this, [&] {
            QElapsedTimer measurement; measurement.start();
            fixture.controller.sidebarStreams();
            maxProjectionMs = qMax(maxProjectionMs, measurement.elapsed()); ++publications;
        });
        QElapsedTimer total; total.start(); loginInbox(fixture, 120);
        QTRY_COMPARE_WITH_TIMEOUT(*gets, 120, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.api.findChildren<QNetworkReply *>().isEmpty(), 30000);
        const auto rows = fixture.controller.sidebarStreams(); QCOMPARE(rows.size(), 120);
        for (const auto &row : rows)
            QCOMPARE(fixture.controller.sidebarTopicState(row.toMap().value("id").toString(), true).value("rows").toList().size(), 20);
        QElapsedTimer projection; projection.start();
        for (int i = 0; i < 20; ++i) fixture.controller.sidebarStreams();
        bool ticked = false; QTimer::singleShot(0, [&] { ticked = true; });
        QTRY_VERIFY_WITH_TIMEOUT(ticked, 1000);
        qInfo() << "POPULATED_EXPANSION_LOAD" << "streams" << ids.size() << "topics-per-stream" << 20
                << "publications" << publications << "max-projection-ms" << maxProjectionMs
                << "projection-20-ms" << projection.elapsed() << "total-ms" << total.elapsed();
    }

    void sidebarExpansionPublishesTopicMovesAndDeletes()
    {
        const auto alpha = QStringLiteral("a4000000-0000-4000-8000-000000000001");
        const auto beta = QStringLiteral("a4000000-0000-4000-8000-000000000002");
        const auto moved = QStringLiteral("a4000000-0000-4000-8000-000000000003");
        Fixture fixture;
        fixture.streams = [=](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 0, 0, 0), inboxStream(beta, "Beta", 0, 0, 0)}}; };
        fixture.topics = [=](const Request &request) {
            const auto id = QUrlQuery(request.target).queryItemValue("stream_uuid");
            return Response{id == alpha ? QJsonArray{inboxTopic(moved, alpha, "Original", 0, 0, 0)} : QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.requestSidebarTopics(alpha); fixture.controller.requestSidebarTopics(beta);
        QTRY_VERIFY(fixture.controller.sidebarTopicState(alpha, false).value("known").toBool());
        QTRY_VERIFY(fixture.controller.sidebarTopicState(beta, false).value("known").toBool());
        QSignalSpy publications(&fixture.controller, &NativeController::sidebarTopicsChanged);
        auto changed = inboxTopic(moved, beta, "Moved", 0, 0, 0, "2026-06-22T11:30:00Z");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", changed, 40)));
        QVERIFY(!publications.isEmpty());
        QVERIFY(fixture.controller.sidebarTopicState(alpha, true).value("rows").toList().isEmpty());
        const auto movedRows = fixture.controller.sidebarTopicState(beta, true).value("rows").toList();
        QCOMPARE(movedRows.size(), 1); QCOMPARE(movedRows[0].toMap().value("name").toString(), QString("Moved"));
        publications.clear();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", moved}, {"stream_uuid", beta}}, 41)));
        QVERIFY(!publications.isEmpty());
        QVERIFY(fixture.controller.sidebarTopicState(beta, true).value("rows").toList().isEmpty());
    }

    void sidebarSearchKeepsHydrationOwnedAndDeduplicated_data()
    {
        QTest::addColumn<bool>("logoutWhileHeld");
        QTest::newRow("current-owner") << false;
        QTest::newRow("logout-before-response") << true;
    }

    void sidebarSearchKeepsHydrationOwnedAndDeduplicated()
    {
        QFETCH(bool, logoutWhileHeld);
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Engineering", 0, 0, 0)}}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto gets = std::make_shared<int>(0);
        fixture.topics = [gates, gets](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") != streamId) return Response{QJsonArray{}, 400};
            ++*gets;
            Response response{QJsonArray{inboxTopic(topicId, streamId, "Planning", 0, 0, 0)}};
            response.defer = [gates](auto send) { gates->push_back(std::move(send)); };
            return response;
        };
        loginInbox(fixture, 1);
        QCOMPARE(*gets, 0);
        for (const auto &query : {QString("p"), QString("pl"), QString("planning")})
            fixture.controller.setSidebarSearchQuery(query);
        QTRY_VERIFY(*gets == 1 && gates->size() == 1);
        QVERIFY(fixture.controller.selectedStream().isEmpty());
        QVERIFY(fixture.controller.selectedTopic().isEmpty());
        QVERIFY(fixture.controller.sidebarStreams().isEmpty());
        if (logoutWhileHeld) {
            fixture.controller.logout();
            QTRY_VERIFY(!fixture.controller.authenticated());
        }
        (*gates)[0](); gates->clear();
        if (logoutWhileHeld) {
            QTest::qWait(200);
            QVERIFY(fixture.controller.sidebarStreams().isEmpty());
            return;
        }
        QTRY_COMPARE(fixture.controller.sidebarStreams().size(), 1);
        QCOMPARE(fixture.controller.sidebarStreams().first().toMap().value("searchTopics").toList()
            .first().toMap().value("id").toString(), topicId);
        fixture.controller.setSidebarSearchQuery("unmatched");
        QVERIFY(fixture.controller.sidebarStreams().isEmpty());
        fixture.controller.setSidebarSearchQuery(" #PLANNING ");
        QCOMPARE(fixture.controller.sidebarStreams().size(), 1);
        QCOMPARE(*gets, 1);
        auto changed = inboxTopic(topicId, streamId, "Renamed", 0, 0, 0, "2026-06-22T11:10:00Z");
        QSignalSpy updates(&fixture.controller, &NativeController::sidebarStreamsChanged);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", changed, 20)));
        QVERIFY(!updates.isEmpty()); QVERIFY(fixture.controller.sidebarStreams().isEmpty());
        fixture.controller.setSidebarSearchQuery("renamed");
        QCOMPARE(fixture.controller.sidebarStreams().size(), 1);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted", changed, 21)));
        QVERIFY(fixture.controller.sidebarStreams().isEmpty());
        fixture.controller.setSidebarSearchQuery("  ");
        QCOMPARE(fixture.controller.sidebarStreams().size(), 1);
        QVERIFY(fixture.controller.selectedStream().isEmpty());
    }

    void sidebarSearchFollowsParticipantEmailUpdates()
    {
        Fixture fixture;
        const QString peer = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        auto user = QJsonObject{{"uuid", peer}, {"username", "alice"}, {"first_name", "Alice"},
            {"last_name", "Ng"}, {"email", "alice@example.invalid"}};
        fixture.users = [user](const Request &) { return Response{QJsonArray{user,
            QJsonObject{{"uuid", userId}, {"username", "cassi"}, {"first_name", "Cassandra"},
                {"last_name", "Volkova"}, {"email", "viewer@example.invalid"}}}}; };
        fixture.streams = [peer](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Private conversation", 0, 0, 0, true, peer)}}; };
        loginInbox(fixture, 1);
        for (const auto &query : {QString("alice ng"), QString(" ALICE@EXAMPLE.INVALID "),
                QString("cassandra volkova"), QString("viewer@example.invalid"), QString("private conversation")}) {
            fixture.controller.setSidebarSearchQuery(query);
            QCOMPARE(fixture.controller.sidebarStreams().size(), 1);
        }
        fixture.controller.setSidebarSearchQuery("alice@example.invalid");
        user["email"] = "new@example.invalid";
        QSignalSpy updates(&fixture.controller, &NativeController::sidebarStreamsChanged);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", user, 20)));
        QVERIFY(!updates.isEmpty()); QVERIFY(fixture.controller.sidebarStreams().isEmpty());
        fixture.controller.setSidebarSearchQuery("new@example.invalid");
        QCOMPARE(fixture.controller.sidebarStreams().size(), 1);
        fixture.controller.logout(); QTRY_VERIFY(!fixture.controller.authenticated());
        QVERIFY(fixture.controller.sidebarStreams().isEmpty());
    }

    // Issue #847 (workspace_ui e0194ffb sidebar-workspace-search.lib.ts,
    // sidebar-workspace.ui.tsx:659-689): a query inside a server folder projects
    // the local matches first, then an "All folders" separator (or the muted
    // no-matches-in-folder status line when the local section is empty), then
    // the global matches deduplicated by stream. The "all" system folder has
    // no local/global split at all, and a query with no matches keeps the list
    // truly empty so the empty state renders.
    void sidebarSearchProjectsCrossFolderSectionsAndSeparators()
    {
        Fixture fixture;
        const QString custom = QStringLiteral("9f9f9f9f-9f9f-4f9f-8f9f-9f9f9f9f9f9f");
        const QString beta = QStringLiteral("8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e");
        const QString gamma = QStringLiteral("7d7d7d7d-7d7d-4d7d-8d7d-7d7d7d7d7d7d");
        fixture.folders = [=](const Request &) { return Response{QJsonArray{
            folderRailFolder(custom, "Design", "created", 0, 0,
                QJsonArray{folderRailItem(streamId), folderRailItem(gamma)})}}; };
        fixture.streams = [=](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Alpha Needle", 0, 0, 0),
            inboxStream(beta, "Beta Needle", 0, 0, 0),
            inboxStream(gamma, "Gamma Needle", 0, 0, 0)}}; };
        loginInbox(fixture, 3);
        // Folder membership arrives asynchronously (same contract the folder
        // rail tests wait on): synthesized system trio + the custom folder.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.folders().size(), 4, 4000);
        // Default "all" folder: flat projection, never any marker row.
        fixture.controller.setSidebarSearchQuery("needle");
        const QStringList flatRows{streamId, beta, gamma};
        QCOMPARE(sidebarRowIds(fixture.controller.sidebarStreams()), flatRows);
        // Server folder: local section, all-folders separator, global tail
        // deduplicated against the local section.
        fixture.controller.selectFolder(custom);
        auto rows = fixture.controller.sidebarStreams();
        QCOMPARE(rows.size(), 4);
        const QVariantList localRows{rows.at(0), rows.at(1)};
        const QStringList localIds{streamId, gamma};
        QCOMPARE(sidebarRowIds(localRows), localIds);
        QCOMPARE(rows.at(2).toMap().value("searchSeparator").toString(), QString("allFolders"));
        QVERIFY(!rows.at(2).toMap().contains("id"));
        QCOMPARE(rows.at(3).toMap().value("id").toString(), beta);
        // Global-only matches: the status line replaces the separator.
        fixture.controller.setSidebarSearchQuery("beta");
        rows = fixture.controller.sidebarStreams();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toMap().value("searchStatus").toString(), QString("noMatchesInFolder"));
        QCOMPARE(rows.at(1).toMap().value("id").toString(), beta);
        // Local-only matches: no marker rows at all.
        fixture.controller.setSidebarSearchQuery("gamma");
        rows = fixture.controller.sidebarStreams();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.at(0).toMap().value("id").toString(), gamma);
        // Nothing matches: the list stays truly empty (empty state renders).
        fixture.controller.setSidebarSearchQuery("nothing");
        QVERIFY(fixture.controller.sidebarStreams().isEmpty());
        // Clearing the query restores the ordinary folder sidebar.
        fixture.controller.setSidebarSearchQuery("");
        const QStringList folderRows{streamId, gamma};
        QCOMPARE(sidebarRowIds(fixture.controller.sidebarStreams()), folderRows);
    }

    void inboxGroupsDirectAndChannelsExcludingPassiveTraffic()
    {
        Fixture fixture;
        fixture.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")},
                {"username", "alice"}, {"first_name", "Alice"}, {"email", "alice@example.invalid"}}}}; };
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000001"), "alice", 2, 2, 0,
                true, QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
            inboxStream(streamId, "Product team", 4, 4, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Quiet", 5, 0, 5),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 2, 2, 0)}}; };
        fixture.topics = [](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == QStringLiteral("a1000000-0000-4000-8000-000000000001"))
                // A blank topic title renders inbox.allMessages (inbox-page.ui.tsx:64-73).
                return Response{QJsonArray{inboxTopic(
                    QStringLiteral("b1000000-0000-4000-8000-000000000001"),
                    QStringLiteral("a1000000-0000-4000-8000-000000000001"), "  ", 2, 2, 0)}};
            if (stream == streamId)
                return Response{QJsonArray{
                    inboxTopic(topicId, streamId, "General Topic", 3, 3, 0, "2026-06-22T10:10:00Z"),
                    inboxTopic(secondTopicId, streamId, "Planning", 0, 0, 0),
                    inboxTopic(QStringLiteral("b1000000-0000-4000-8000-000000000002"), streamId,
                        "Releases", 1, 1, 0, "2026-06-22T15:00:00Z")}};
            if (stream == QStringLiteral("a1000000-0000-4000-8000-000000000002"))
                return Response{QJsonArray{inboxTopic(
                    QStringLiteral("b1000000-0000-4000-8000-000000000003"),
                    QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Lurk", 5, 0, 5)}};
            if (stream == QStringLiteral("a1000000-0000-4000-8000-000000000003"))
                return Response{QJsonArray{inboxTopic(
                    QStringLiteral("b1000000-0000-4000-8000-000000000004"),
                    QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Bulletin", 0, 0, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 4);
        const auto before = fixture.controller.inboxPage();
        QVERIFY(!before.value("loading").toBool());
        QVERIFY(!before.value("error").toBool());
        QVERIFY(!fixture.controller.inboxVisible());
        fixture.controller.showInbox();
        QVERIFY(fixture.controller.inboxVisible());
        // Topic rows refine once the per-stream catalogs land (lazy native
        // hydration documented in the design doc).
        QVERIFY(QTest::qWaitFor([&fixture] {
            const auto page = fixture.controller.inboxPage();
            const auto dm = inboxCard(page, "direct",
                QStringLiteral("a1000000-0000-4000-8000-000000000001"));
            const auto dmRows = dm.value("rows").toList();
            const auto productRows = inboxCard(page, "channels", streamId).value("rows").toList();
            return dmRows.size() == 1 && dmRows.at(0).toMap().value("topic").toString()
                       == QStringLiteral("b1000000-0000-4000-8000-000000000001")
                && productRows.size() == 2;
        }, 4000));
        const auto page = fixture.controller.inboxPage();

        const auto direct = page.value("direct").toList();
        QCOMPARE(direct.size(), 1);
        const auto dmCard = direct.first().toMap();
        QCOMPARE(dmCard.value("id").toString(),
            QStringLiteral("a1000000-0000-4000-8000-000000000001"));
        QCOMPARE(dmCard.value("title").toString(), QStringLiteral("Alice")); // peer display name
        QVERIFY(dmCard.value("direct").toBool());
        // Card badge = stream active unread (inbox-page.ui.tsx:158).
        QCOMPARE(dmCard.value("badge").toString(), QStringLiteral("2"));
        const auto dmRows = dmCard.value("rows").toList();
        QCOMPARE(dmRows.size(), 1);
        const auto dmRow = dmRows.first().toMap();
        QCOMPARE(dmRow.value("title").toString(), QStringLiteral("Alice · All messages"));
        QCOMPARE(dmRow.value("topic").toString(),
            QStringLiteral("b1000000-0000-4000-8000-000000000001"));
        QCOMPARE(dmRow.value("badge").toString(), QStringLiteral("2"));
        QVERIFY(!dmRow.value("time").toString().isEmpty());

        // The passive-only stream stays out of the inbox entirely
        // (hasUnread, inbox-page.ui.tsx:54-61; resolveWorkspaceActiveUnreadCount
        // messenger-notification-mode.lib.ts:37-41).
        const auto channels = page.value("channels").toList();
        QCOMPARE(channels.size(), 2);
        const auto product = channels.at(0).toMap();
        QCOMPARE(product.value("id").toString(), streamId);
        QCOMPARE(product.value("badge").toString(), QStringLiteral("4"));
        const auto productRows = product.value("rows").toList();
        QCOMPARE(productRows.size(), 2); // unread topics only (inbox-page.ui.tsx:86-90)
        // Recency order: Releases (15:00Z) before General Topic (10:10Z).
        QCOMPARE(productRows.at(0).toMap().value("title").toString(),
            QStringLiteral("Product team · Releases"));
        QCOMPARE(productRows.at(1).toMap().value("title").toString(),
            QStringLiteral("Product team · General Topic"));
        const auto notice = channels.at(1).toMap();
        QCOMPARE(notice.value("id").toString(),
            QStringLiteral("a1000000-0000-4000-8000-000000000003"));
        // Stream-level unread with all topics read renders ONE fallback row
        // (buildInboxStreamGroup, inbox-page.ui.tsx:86-95).
        const auto noticeRows = notice.value("rows").toList();
        QCOMPARE(noticeRows.size(), 1);
        QCOMPARE(noticeRows.first().toMap().value("title").toString(), QStringLiteral("Notice"));
        QVERIFY(noticeRows.first().toMap().value("topic").toString().isEmpty());
        QCOMPARE(noticeRows.first().toMap().value("badge").toString(), QStringLiteral("2"));

        fixture.controller.closeInbox();
        QVERIFY(!fixture.controller.inboxVisible());
    }

    void inboxRowOpensTargetAndClosesPage()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 3, 3, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 2, 2, 0)}}; };
        fixture.topics = [](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == streamId)
                return Response{QJsonArray{inboxTopic(topicId, streamId, "General Topic", 3, 3, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Wait for the refined topic row (the slot owns the canonical DTO
        // that openInboxRow hands to the topic-opening machinery).
        QVERIFY(QTest::qWaitFor([&fixture] {
            const auto rows = inboxCard(fixture.controller.inboxPage(), "channels", streamId)
                .value("rows").toList();
            return rows.size() == 1 && rows.at(0).toMap().value("topic").toString() == topicId;
        }, 4000));
        // A topic row opens the topic and the page unmounts (reference row click
        // navigates, inbox-page.ui.tsx:108-110).
        fixture.controller.openInboxRow(streamId, topicId);
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == topicId, 4000);

        fixture.controller.showInbox();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxVisible(), 2000);
        fixture.controller.closeInbox();
        // The fallback row has no topic target and selects the stream.
        fixture.controller.openInboxRow(QStringLiteral("a1000000-0000-4000-8000-000000000003"), QString());
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedStream()
            == QStringLiteral("a1000000-0000-4000-8000-000000000003"), 4000);
    }

    void inboxBadgeFormattingMatchesReferenceLimit()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000005"), "Thousand", 1000, 1000, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000006"), "Over", 1001, 1001, 0)}}; };
        fixture.topics = [](const Request &) { return Response{QJsonArray{}}; };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // UNREAD_COUNT_DISPLAY_LIMIT = 1000 with floor(count/1000)+"k+" only
        // above it (count-format.lib.ts): 1000 -> "1000", 1001 -> "1k+".
        const auto thousand = inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000005"));
        const auto over = inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000006"));
        QCOMPARE(thousand.value("badge").toString(), QStringLiteral("1000"));
        QCOMPARE(over.value("badge").toString(), QStringLiteral("1k+"));
        QCOMPARE(thousand.value("rows").toList().at(0).toMap().value("badge").toString(),
            QStringLiteral("1000"));
        QCOMPARE(over.value("rows").toList().at(0).toMap().value("badge").toString(),
            QStringLiteral("1k+"));
    }

    void inboxRealtimeCounterUpdateRecomputesPage()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 3, 3, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Quiet", 0, 0, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 2, 2, 0)}}; };
        fixture.topics = [](const Request &) { return Response{QJsonArray{}}; };
        loginInbox(fixture, 3);
        fixture.controller.showInbox();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxPage().value("channels").toList().size() == 2, 4000);

        // stream.updated raises the muted stream's active counters -> it joins
        // the inbox (reference store applies the replaced counters live).
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Quiet", 7, 7, 0), 20)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxPage().value("channels").toList().size() == 3, 4000);
        QVERIFY(!inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000002")).isEmpty());

        // stream.read zeroes the counters -> the stream leaves the inbox.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.read",
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 0, 0, 0), 21)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxPage().value("channels").toList().size() == 2, 4000);
        QVERIFY(inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000003")).isEmpty());
    }

    // Issue #65: a whole-topic move (a complete owned topic.updated whose
    // stream_uuid differs from the slot that holds the topic) must reconcile
    // every Inbox slot: the vacated card goes away, exactly one row survives
    // at the destination, and navigation receives the current placement,
    // never the stale source DTO (openInboxRow hands the slot DTO to
    // beginTopicOpening as verified canonical metadata).
    void inboxTopicMoveAcrossStreamsReconcilesSlots()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        const auto nightly = QStringLiteral("b1000000-0000-4000-8000-0000000000d4");
        Fixture fixture;
        fixture.streams = [=](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 1, 1, 0)}}; };
        fixture.topics = [=](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha)
                return Response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0, "2026-06-22T09:00:00Z"),
                    inboxTopic(secondTopicId, alpha, "Planning", 0, 0, 0)}};
            if (stream == beta)
                return Response{QJsonArray{
                    inboxTopic(nightly, beta, "Nightly", 1, 1, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&fixture, alpha, moved] {
            return !inboxRow(fixture.controller.inboxPage(), "channels", alpha, moved).isEmpty();
        }, 4000));

        // The complete owned move of the still-unread topic to Beta, followed
        // by the stream counter updates the server pairs the move with.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 40)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 4, 4, 0), 41)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 42)));
        // Exactly one row survives, under Beta with the moved badge; Alpha
        // leaves the inbox once no unread topics or stream messages remain.
        QVERIFY(QTest::qWaitFor([&fixture, alpha, beta, moved] {
            const auto page = fixture.controller.inboxPage();
            return inboxCard(page, "channels", alpha).isEmpty()
                && inboxRow(page, "channels", beta, moved).value("badge").toString()
                    == QStringLiteral("3");
        }, 4000));
        QCOMPARE(inboxCard(fixture.controller.inboxPage(), "channels", beta)
            .value("rows").toList().size(), 2); // Nightly + the moved topic

        // Clicking the surviving row opens the topic at its current home.
        fixture.controller.openInboxRow(beta, moved);
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == moved, 4000);
        fixture.controller.showInbox();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxVisible(), 2000);

        // A rename at the destination updates the single row in place.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration v2", 3, 3, 0, "2026-06-22T12:00:00Z"), 43)));
        QVERIFY(QTest::qWaitFor([&fixture, beta, moved] {
            const auto rows = inboxCard(fixture.controller.inboxPage(), "channels", beta)
                .value("rows").toList();
            return rows.size() == 2 && rows.at(0).toMap().value("title").toString()
                == QStringLiteral("Beta · Migration v2");
        }, 4000));

        // Moving the topic back revives Alpha and vacates Beta.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, alpha, "Migration v2", 3, 3, 0, "2026-06-22T13:00:00Z"), 44)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 3, 3, 0), 45)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 1, 1, 0), 46)));
        QVERIFY(QTest::qWaitFor([&fixture, alpha, beta, moved] {
            const auto page = fixture.controller.inboxPage();
            const auto alphaRows = inboxCard(page, "channels", alpha).value("rows").toList();
            const auto betaRows = inboxCard(page, "channels", beta).value("rows").toList();
            return alphaRows.size() == 1
                && alphaRows.at(0).toMap().value("topic").toString() == moved
                && betaRows.size() == 1;
        }, 4000));

        // Later read and delete events affect the surviving destination row.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.read",
            inboxTopic(moved, alpha, "Migration v2", 0, 0, 0, "2026-06-22T13:30:00Z"), 47)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.read",
            inboxStream(alpha, "Alpha", 0, 0, 0), 48)));
        QVERIFY(QTest::qWaitFor([&fixture, alpha] {
            return inboxCard(fixture.controller.inboxPage(), "channels", alpha).isEmpty();
        }, 4000));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", moved}, {"stream_uuid", alpha}}, 49)));
        // The tombstone leaves no row anywhere and does not disturb Beta.
        QTest::qWait(250);
        const auto page = fixture.controller.inboxPage();
        QVERIFY(inboxCard(page, "channels", alpha).isEmpty());
        QCOMPARE(inboxCard(page, "channels", beta).value("rows").toList().size(), 1);
    }

    // Issue #65: the destination slot may not be hydrated yet (the stream had
    // no unread when the Inbox opened). The move must invalidate toward the
    // destination and refetch its complete slot instead of fabricating a
    // partial one; the fetched snapshot, not the event DTO, owns the row.
    void inboxTopicMoveToUnhydratedStreamRefetchesDestination()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        Fixture fixture;
        fixture.streams = [=](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 0, 0, 0)}}; };
        fixture.topics = [=](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha)
                return Response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0)}};
            if (stream == beta)
                // Server-side truth after the move. The counters deliberately
                // differ from the event DTO so a refetch cannot be confused
                // with a direct upsert of the event payload.
                return Response{QJsonArray{
                    inboxTopic(moved, beta, "Migration", 4, 4, 0, "2026-06-22T11:45:00Z")}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Beta carries no unread at open time, so only Alpha hydrates a slot.
        QVERIFY(QTest::qWaitFor([&fixture, alpha, moved] {
            return !inboxRow(fixture.controller.inboxPage(), "channels", alpha, moved).isEmpty();
        }, 4000));
        QVERIFY(inboxCard(fixture.controller.inboxPage(), "channels", beta).isEmpty());

        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 50)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 3, 3, 0), 51)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 52)));
        // Alpha's stale row disappears and the refetched Beta slot owns the
        // complete destination state (the fetched badge 4, not the event's 3).
        QVERIFY(QTest::qWaitFor([&fixture, alpha, beta, moved] {
            const auto page = fixture.controller.inboxPage();
            return inboxCard(page, "channels", alpha).isEmpty()
                && inboxRow(page, "channels", beta, moved).value("badge").toString()
                    == QStringLiteral("4");
        }, 4000));
        fixture.controller.openInboxRow(beta, moved);
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == moved, 4000);
    }

    // Reviewer follow-up on #65: a slot snapshot generated before a placement
    // event must not resurrect the old placement when it lands after it. The
    // first Alpha fetch is held in flight (deterministic defer gate), the move
    // A->B is applied, then the stale snapshot is released.
    void delayedInboxSlotCannotResurrectMovedTopic()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        const auto nightly = QStringLiteral("b1000000-0000-4000-8000-0000000000d4");
        Fixture fixture;
        std::function<void()> releaseAlpha;
        bool gated = false;
        fixture.streams = [&](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 1, 1, 0)}}; };
        fixture.topics = [&](const Request &request) -> Response {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha) {
                // The pre-move snapshot: still lists the topic under Alpha.
                Response response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0, "2026-06-22T09:00:00Z"),
                    inboxTopic(secondTopicId, alpha, "Planning", 0, 0, 0)}};
                if (!gated) {
                    gated = true;
                    response.defer = [&](std::function<void()> send) { releaseAlpha = std::move(send); };
                }
                return response;
            }
            if (stream == beta)
                return Response{QJsonArray{inboxTopic(nightly, beta, "Nightly", 1, 1, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Alpha's slot response is held in flight; Beta's slot lands first.
        QVERIFY(QTest::qWaitFor([&fixture, &gated, beta, nightly] {
            return gated && !inboxRow(fixture.controller.inboxPage(), "channels", beta, nightly).isEmpty();
        }, 4000));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 60)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 4, 4, 0), 61)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 62)));
        QVERIFY(QTest::qWaitFor([&fixture, beta, moved] {
            return inboxRow(fixture.controller.inboxPage(), "channels", beta, moved)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
        // The stale snapshot lands after the move: it must not revive Alpha.
        releaseAlpha();
        QTest::qWait(300);
        const auto page = fixture.controller.inboxPage();
        QVERIFY2(inboxCard(page, "channels", alpha).isEmpty(),
            "An in-flight slot snapshot restored the topic to its vacated stream");
        QCOMPARE(inboxRow(page, "channels", beta, moved).value("badge").toString(),
            QStringLiteral("3"));
    }

    // Reviewer follow-up (PR #75): the placement overlay supplements a fetch
    // that started AFTER the event too. When the fetch reflects a later read
    // whose updated_at ties with the event's, the snapshot counters must win
    // - the topic left the inbox and must not reappear unread.
    void inboxSlotSnapshotCountersWinOverEqualTimestampPlacementEvent()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        Fixture fixture;
        fixture.streams = [&](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 0, 0, 0)}}; };
        fixture.topics = [&](const Request &request) -> Response {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha)
                return Response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z")}};
            if (stream == beta)
                // Fetched after the move event: the topic was read while
                // resolving; the server keeps the same updated_at.
                return Response{QJsonArray{
                    inboxTopic(moved, beta, "Migration", 0, 0, 0, "2026-06-22T11:30:00Z")}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Beta carries no unread at open, so only Alpha hydrates a slot.
        QVERIFY(QTest::qWaitFor([&fixture, alpha, moved] {
            return !inboxRow(fixture.controller.inboxPage(), "channels", alpha, moved).isEmpty();
        }, 4000));
        // The move event arrives at 11:30Z with unread counters; the
        // destination refetch then answers with the fresher read state at
        // the same updated_at.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 80)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 3, 3, 0), 81)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 82)));
        QVERIFY(QTest::qWaitFor([&fixture, alpha] {
            return inboxCard(fixture.controller.inboxPage(), "channels", alpha).isEmpty();
        }, 4000));
        // The topic is read at its destination: no topic row may appear.
        QTest::qWait(250);
        QVERIFY2(inboxRow(fixture.controller.inboxPage(), "channels", beta, moved).isEmpty(),
            "A tie on updated_at let the event DTO overwrite fresher read counters");
    }

    void delayedInboxSlotCannotResurrectDeletedTopic()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        Fixture fixture;
        std::function<void()> releaseAlpha;
        bool gated = false;
        fixture.streams = [&](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0)}}; };
        fixture.topics = [&](const Request &request) -> Response {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha) {
                Response response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0),
                    inboxTopic(secondTopicId, alpha, "Planning", 0, 0, 0)}};
                if (!gated) {
                    gated = true;
                    response.defer = [&](std::function<void()> send) { releaseAlpha = std::move(send); };
                }
                return response;
            }
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        // Alpha's slot response is held in flight.
        QVERIFY(QTest::qWaitFor([&gated] { return gated; }, 4000));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", moved}, {"stream_uuid", alpha}}, 70)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 71)));
        releaseAlpha();
        QTest::qWait(300);
        QVERIFY2(inboxCard(fixture.controller.inboxPage(), "channels", alpha).isEmpty(),
            "An in-flight slot snapshot resurrected a deleted topic");
    }

    void inboxDeeplinkOpensTheInboxPage()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 1, 1, 0)}}; };
        loginInbox(fixture, 1);
        const auto origin = fixture.server.origin();
        // /inbox is allowlisted (native-deeplink.cpp:39); the reference
        // navigates the inbox page for it.
        fixture.controller.openMessageLink(QStringLiteral("ew://open/org/127.0.0.1-")
            + QString::number(origin.port()) + "/project/" + projectId + "/inbox");
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxVisible(), 4000);
        fixture.controller.closeInbox();
        // A foreign project route keeps the safe no-navigation behaviour.
        fixture.controller.openMessageLink(QStringLiteral("ew://open/org/127.0.0.1-")
            + QString::number(origin.port())
            + "/project/99999999-9999-4999-8999-999999999999/inbox");
        QTest::qWait(250);
        QVERIFY(!fixture.controller.inboxVisible());
    }

    void inboxEmptyAndErrorStates()
    {
        Fixture ok;
        ok.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 0, 0, 0)}}; };
        loginInbox(ok, 1);
        ok.controller.showInbox();
        const auto clean = ok.controller.inboxPage();
        QVERIFY(!clean.value("loading").toBool());
        QVERIFY(!clean.value("error").toBool());
        QVERIFY(clean.value("direct").toList().isEmpty() && clean.value("channels").toList().isEmpty());

        // A failed catalog keeps m_catalogVerified false: loading resolves to
        // the error card (inbox-page.ui.tsx:196-207 state derivation).
        Fixture failing;
        failing.streams = [](const Request &) { return Response{QJsonArray{}, 500}; };
        failing.controller.login(failing.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&failing] { return failing.controller.projects().size() == 1; }, 2000));
        failing.controller.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&failing] {
            return failing.controller.streams().isEmpty() && failing.controller.authenticated()
                && !failing.controller.error().isEmpty(); }, 3000));
        failing.controller.showInbox();
        const auto page = failing.controller.inboxPage();
        QVERIFY(page.value("error").toBool());
        QVERIFY(!page.value("loading").toBool());
    }

    // Issue #64: a failed first topic-list request must not pin the stream in
    // the hydration set. Closing and reopening Inbox must retry the request and
    // refine the fallback card into topic rows (docs/NATIVE_DESKTOP_INBOX.md
    // failure-recovery section; acceptance criteria in the issue).
    void inboxFailedTopicHydrationRetriesOnReopen()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 4, 4, 0)}}; };
        auto topicGets = std::make_shared<int>(0);
        fixture.topics = [stream, topicGets](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            if (++*topicGets == 1)
                return Response{QJsonArray{}, 500}; // transient server error
            return Response{QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)}};
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets >= 1; }, 4000));
        QTest::qWait(250); // let the failure settle before inspecting the page
        // The failed stream keeps only the stream-level fallback row.
        const auto failedRows = inboxCard(fixture.controller.inboxPage(), "channels", stream)
            .value("rows").toList();
        QCOMPARE(failedRows.size(), 1);
        QVERIFY(failedRows.at(0).toMap().value("topic").toString().isEmpty());
        // Close + reopen must re-dispatch the hydration request (#64).
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets >= 2; }, 4000));
        QVERIFY(QTest::qWaitFor([&] {
            return inboxRow(fixture.controller.inboxPage(), "channels", stream, topicId)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
    }

    // Issue #64 acceptance: an unsuccessful later pagination page releases the
    // hydration marker just like a first-page failure.
    void inboxFailedTopicPaginationReleasesHydration()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        const auto lateTopic = QStringLiteral("b1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 5, 5, 0)}}; };
        auto cycles = std::make_shared<int>(0);
        auto failedOnce = std::make_shared<bool>(false);
        fixture.topics = [stream, lateTopic, cycles, failedOnce](const Request &request) {
            const QUrlQuery query(request.target);
            if (query.queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) ++*cycles;
            if (*cycles == 1 && !marker.isEmpty()) {
                *failedOnce = true;
                return Response{QJsonArray{}, 500}; // second page fails
            }
            if (marker.isEmpty())
                return Response{QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)},
                    200, 0, {{"X-Pagination-Marker", "p2"}}};
            return Response{QJsonArray{inboxTopic(lateTopic, stream, "Release notes", 2, 2, 0,
                "2026-06-22T11:00:00Z")}};
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *failedOnce; }, 4000));
        QTest::qWait(250);
        // A mid-pagination failure leaves the slot unhydrated: fallback row only.
        const auto failedRows = inboxCard(fixture.controller.inboxPage(), "channels", stream)
            .value("rows").toList();
        QCOMPARE(failedRows.size(), 1);
        QVERIFY(failedRows.at(0).toMap().value("topic").toString().isEmpty());
        // Reopen retries from page one and lands both pages.
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] {
            const auto page = fixture.controller.inboxPage();
            return inboxRow(page, "channels", stream, topicId).contains("badge")
                && inboxRow(page, "channels", stream, lateTopic).contains("badge");
        }, 4000));
    }

    // Issue #64 acceptance: a successful hydration that is still in flight stays
    // deduplicated across a close/reopen cycle (no duplicate collection).
    void inboxInFlightHydrationStaysDeduplicatedOnReopen()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 4, 4, 0)}}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto topicGets = std::make_shared<int>(0);
        fixture.topics = [stream, gates, topicGets](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            ++*topicGets;
            Response response{QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)}};
            response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
            return response;
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets == 1 && gates->size() == 1; }, 4000));
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QTest::qWait(250);
        QCOMPARE(*topicGets, 1); // the in-flight hydration was not re-dispatched
        (*gates)[0]();
        QVERIFY(QTest::qWaitFor([&] {
            return inboxRow(fixture.controller.inboxPage(), "channels", stream, topicId)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
    }

    // Issue #64 acceptance: a stale session's late failure must not release the
    // NEW session's in-flight marker, so the new hydration stays deduplicated.
    void inboxStaleHydrationFailureCannotPoisonNewSession()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 4, 4, 0)}}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto topicGets = std::make_shared<int>(0);
        fixture.topics = [stream, gates, topicGets](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            ++*topicGets;
            Response response{*topicGets == 1 ? QJsonArray{}
                : QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)},
                *topicGets == 1 ? 500 : 200};
            response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
            return response;
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets == 1 && gates->size() == 1; }, 4000));
        fixture.controller.logout();
        QVERIFY(QTest::qWaitFor([&fixture] { return !fixture.controller.authenticated(); }, 4000));
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets == 2 && gates->size() == 2; }, 4000));
        // The old session's GET fails now, after the new session started its own.
        (*gates)[0]();
        gates->erase(gates->begin());
        QTest::qWait(250);
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QTest::qWait(250);
        QCOMPARE(*topicGets, 2); // the stale failure must not free the new marker
        (*gates)[0]();
        QVERIFY(QTest::qWaitFor([&] {
            return inboxRow(fixture.controller.inboxPage(), "channels", stream, topicId)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
    }

    // Issue #54: opening a sidebar destination closes the previously opened
    // page, so exactly one page (and one sidebar selection) stays active and
    // closing the active page returns directly to the conversation. Draft
    // contents survive the transitions. The reference routes one destination
    // at a time (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af router:
    // each of inbox-page.ui.tsx, activity-mentions.ui.tsx and the drafts page
    // replaces the previous destination instead of stacking beside it).
    void draftsMentionsInboxNavigationIsMutuallyExclusive()
    {
        QTemporaryDir draftDirectory; QVERIFY(draftDirectory.isValid());
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(
            QMap<int, QJsonObject>{{1, mentionMessage(1, false)}});
        fixture.mentions = [rows](const Request &request) { return mentionPageResponse(*rows, request); };
        fixture.controller.useDrafts(&drafts);
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Kept across navigation");
        QCOMPARE(fixture.controller.draft(), QStringLiteral("Kept across navigation"));

        fixture.controller.showDrafts();
        QVERIFY(fixture.controller.draftsVisible());
        QTRY_VERIFY(!fixture.controller.draftPage().value("rows").toList().isEmpty());
        // Drafts -> Mentions closes the drafts page.
        fixture.controller.showMentions();
        QVERIFY(fixture.controller.mentionsVisible());
        QVERIFY(!fixture.controller.draftsVisible());
        QVERIFY(!fixture.controller.inboxVisible());
        // Mentions -> Drafts closes mentions and the draft survives the round trip.
        fixture.controller.showDrafts();
        QVERIFY(fixture.controller.draftsVisible());
        QVERIFY(!fixture.controller.mentionsVisible());
        QCOMPARE(fixture.controller.draft(), QStringLiteral("Kept across navigation"));
        // Inbox shares the same exclusivity with both pages.
        fixture.controller.showInbox();
        QVERIFY(fixture.controller.inboxVisible());
        QVERIFY(!fixture.controller.draftsVisible());
        fixture.controller.showMentions();
        QVERIFY(fixture.controller.mentionsVisible());
        QVERIFY(!fixture.controller.inboxVisible());
        fixture.controller.showInbox();
        QVERIFY(fixture.controller.inboxVisible());
        QVERIFY(!fixture.controller.mentionsVisible());
        // Closing the one active page returns to the conversation: no page open.
        fixture.controller.closeInbox();
        QVERIFY(!fixture.controller.inboxVisible() && !fixture.controller.draftsVisible()
            && !fixture.controller.mentionsVisible());
        // Draft contents survived every transition.
        QCOMPARE(fixture.controller.draft(), QStringLiteral("Kept across navigation"));
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        drafts.pauseRemote(true);
        bool stopped = false;
        drafts.flush(this, [&stopped](bool) { stopped = true; });
        QTRY_VERIFY(stopped);
        draftStorage.shutdown();
        QTRY_VERIFY(draftStorage.stopped());
    }

    // Issue #35: the top-bar section state switches chat/mail exclusively,
    // mirroring the reference router where /mail is the mail section and any
    // conversation route maps back to chat (top-bar.lib.ts:17-29,
    // getSectionFromPathname).
    void sectionStateSwitchesChatAndMailExclusively()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QSignalSpy changes(&fixture.controller, &NativeController::stateChanged);
        QVERIFY(changes.isValid());
        fixture.controller.showSection(QStringLiteral("mail"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("mail"));
        QVERIFY(changes.count() >= 1);
        // Entering the mail section closes the chat content pages.
        fixture.controller.showInbox();
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QVERIFY(fixture.controller.inboxVisible());
        fixture.controller.showSection(QStringLiteral("mail"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("mail"));
        QVERIFY(!fixture.controller.inboxVisible());
        // Re-opening a chat content page exits the mail section.
        fixture.controller.showMentions();
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QVERIFY(fixture.controller.mentionsVisible());
        // Selecting a conversation is chat navigation.
        fixture.controller.showSection(QStringLiteral("mail"));
        fixture.controller.selectStream(streamId);
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        // Services became a rendered section in issue #45: it takes the top-bar
        // slot from mail exactly like the other sections do. (Calls moved to a
        // rendered section in issue #37.)
        fixture.controller.showSection(QStringLiteral("mail"));
        fixture.controller.showSection(QStringLiteral("services"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("services"));
        fixture.controller.showSection(QStringLiteral("chat"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
    }

    // Issue #36: the top-bar section state switches chat/calendar exclusively,
    // mirroring the reference router where /calendar is the calendar section
    // and any conversation route maps back to chat (top-bar.lib.ts,
    // getTopBarSectionNavItems + resolveTopBarActiveSection at workspace_ui
    // 61dacef78dab2f5d0be76f283bebf49fe29b84af).
    void sectionStateSwitchesChatAndCalendarExclusively()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QSignalSpy changes(&fixture.controller, &NativeController::stateChanged);
        QVERIFY(changes.isValid());
        fixture.controller.showSection(QStringLiteral("calendar"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("calendar"));
        QVERIFY(changes.count() >= 1);
        // Entering the calendar section closes the chat content pages.
        fixture.controller.showInbox();
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QVERIFY(fixture.controller.inboxVisible());
        fixture.controller.showSection(QStringLiteral("calendar"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("calendar"));
        QVERIFY(!fixture.controller.inboxVisible());
        // Re-opening a chat content page exits the calendar section.
        fixture.controller.showMentions();
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QVERIFY(fixture.controller.mentionsVisible());
        // Selecting a conversation is chat navigation.
        fixture.controller.showSection(QStringLiteral("calendar"));
        fixture.controller.selectStream(streamId);
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        // Calendar and mail hold the same slot: each chip takes it from the
        // other, and switching closes the chat content pages.
        fixture.controller.showSection(QStringLiteral("mail"));
        fixture.controller.showSection(QStringLiteral("calendar"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("calendar"));
        QVERIFY(!fixture.controller.draftsVisible() && !fixture.controller.mentionsVisible()
            && !fixture.controller.inboxVisible());
        // Services became a rendered section in issue #45: it takes the
        // top-bar slot from calendar as well.
        fixture.controller.showSection(QStringLiteral("services"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("services"));
        fixture.controller.showSection(QStringLiteral("chat"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
    }

    // Issue #910: navigating the calendar while a window request is in flight
    // must not drop the navigation. The in-flight reply may only populate the
    // window it was sent for; the newest requested window has to be re-issued
    // once the current request settles (deterministic defer-gate repro).
    void calendarNavigationDuringInflightReissuesNewestWindow_data()
    {
        QTest::addColumn<int>("firstStatus");
        QTest::newRow("loaded") << 200;
        QTest::newRow("feed-unavailable") << 501;
        QTest::newRow("request-error") << 503;
    }

    void calendarNavigationDuringInflightReissuesNewestWindow()
    {
        QFETCH(int, firstStatus);
        Fixture fixture;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto queries = std::make_shared<QStringList>();
        fixture.calendar = [gates, queries, firstStatus](const Request &request) {
            queries->append(QUrlQuery(request.target).query(QUrl::FullyEncoded));
            Response response{QJsonArray{}, queries->size() == 1 ? firstStatus : 200};
            response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
            return response;
        };
        QVERIFY(fixture.start());
        fixture.controller.requestCalendarWindow(QStringLiteral("2026-11-01"),
                                                 QStringLiteral("2026-11-30"));
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first window is in flight
        // Navigation lands while the first window is still in flight.
        fixture.controller.requestCalendarWindow(QStringLiteral("2026-12-01"),
                                                 QStringLiteral("2026-12-31"));
        for (auto &release : *gates)
            release();
        gates->clear();
        QTRY_VERIFY_WITH_TIMEOUT(queries->size() == 2, 3000);
        QCOMPARE(calendarFirstDay(QUrlQuery(queries->at(1))), QStringLiteral("2026-12-01"));
        QCOMPARE(calendarLastDay(QUrlQuery(queries->at(1))), QStringLiteral("2026-12-31"));
        // exo-review on #910: settle the re-issued window's reply before
        // re-requesting it, otherwise the final request would exercise the
        // in-flight branch instead of the settled-window guard under test.
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 3000); // the re-issue is gated
        for (auto &release : *gates)
            release();
        gates->clear();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.calendarLoading(), 3000);
        // The settled newest window re-shows without re-fetching (guard intact).
        fixture.controller.requestCalendarWindow(QStringLiteral("2026-12-01"),
                                                 QStringLiteral("2026-12-31"));
        QTest::qWait(200);
        QCOMPARE(queries->size(), 2);
    }

    // Review follow-up on #910: returning to the window that is CURRENTLY in
    // flight makes every queued window stale - the newest requested window is
    // the one being fetched, so the queue must be dropped instead of fired
    // when the in-flight reply settles.
    void calendarReturnToInflightWindowDropsQueuedWindow()
    {
        Fixture fixture;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto queries = std::make_shared<QStringList>();
        fixture.calendar = [gates, queries](const Request &request) {
            queries->append(QUrlQuery(request.target).query(QUrl::FullyEncoded));
            Response response{QJsonArray{}};
            response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
            return response;
        };
        QVERIFY(fixture.start());
        fixture.controller.requestCalendarWindow(QStringLiteral("2026-11-01"),
                                                 QStringLiteral("2026-11-30"));
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first window is in flight
        // A newer window is queued, then the user returns to the in-flight one.
        fixture.controller.requestCalendarWindow(QStringLiteral("2026-12-01"),
                                                 QStringLiteral("2026-12-31"));
        fixture.controller.requestCalendarWindow(QStringLiteral("2026-11-01"),
                                                 QStringLiteral("2026-11-30"));
        for (auto &release : *gates)
            release();
        gates->clear();
        // The in-flight reply settles as the newest requested window; the
        // dropped queue must never fire afterwards.
        QTest::qWait(400);
        QCOMPARE(queries->size(), 1);
        QVERIFY(gates->empty());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.calendarLoading(), 3000);
    }

    // Issue #37: the recent-calls page is a rendered section (calls-page.ui.tsx
    // at workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af). The reference
    // top-bar Calls chip sits behind the TOP_BAR_CALLS_NAV build gate
    // (top-bar.lib.ts:89-91) and the native build settings do not introduce
    // that gate, so the section state and the whitelisted /calls deep link
    // (app-route-definitions.tsx:161-162,198-199) are the entry surfaces.
    void sectionStateRendersRecentCallsSection()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QSignalSpy changes(&fixture.controller, &NativeController::stateChanged);
        QVERIFY(changes.isValid());
        fixture.controller.showSection(QStringLiteral("calls"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("calls"));
        QVERIFY(changes.count() >= 1);
        // Entering the calls section closes the chat content pages, and
        // re-opening a chat content page exits the calls section.
        fixture.controller.showInbox();
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        QVERIFY(fixture.controller.inboxVisible());
        fixture.controller.showSection(QStringLiteral("calls"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("calls"));
        QVERIFY(!fixture.controller.inboxVisible());
        fixture.controller.showMentions();
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        // Selecting a conversation is chat navigation.
        fixture.controller.showSection(QStringLiteral("calls"));
        fixture.controller.selectStream(streamId);
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
        // Calls, mail and calendar hold the same slot: each takes it from the
        // other and closes the chat content pages.
        fixture.controller.showSection(QStringLiteral("mail"));
        fixture.controller.showSection(QStringLiteral("calls"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("calls"));
        QVERIFY(!fixture.controller.draftsVisible() && !fixture.controller.mentionsVisible()
            && !fixture.controller.inboxVisible());
        // The whitelisted /calls deep link opens the section (the entry
        // surface while the top-bar chip stays gated off, like /inbox and
        // /feed). Deferred while the fixture settles, hence QTRY.
        fixture.controller.showSection(QStringLiteral("chat"));
        fixture.controller.openMessageLink(QStringLiteral("ew://open/calls"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.section(), QStringLiteral("calls"), 5000);
        fixture.controller.showSection(QStringLiteral("chat"));
        QCOMPARE(fixture.controller.section(), QStringLiteral("chat"));
    }

    void calendarUnchangedWireTextSurvivesUnrelatedEdits()
    {
        Fixture fixture;
        const auto base = fixture.server.handler;
        const auto id = QStringLiteral("95000000-0000-4000-8000-000000000001");
        auto event = calendarEvent(id, "2026-11-01", "09:00", "10:00", "  Existing title ",
                                   workCalendarId, " Room 4  ");
        auto original = event.value("payload").toObject();
        QJsonObject sent;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/calendar/calendars/") {
                auto row = calendarList().first().toObject();
                row["read_only"] = false;
                row["external_account_uuid"] = QJsonValue::Null;
                return {QJsonArray{row}};
            }
            if (path == "/api/workspace/v1/calendar/events/")
                return {QJsonArray{event}};
            if (request.method == "PUT" && path == "/api/workspace/v1/calendar/events/" + id) {
                sent = request.body.value("payload").toObject();
                event["payload"] = sent;
                return {event};
            }
            return base(request);
        };
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_COMPARE(controller.calendarEvents().size(), 1);
        QVariantMap draft{{"title", "Renamed"}, {"location", original.value("location").toString()},
                          {"startUtc", original.value("starts_at").toString()},
                          {"endUtc", original.value("ends_at").toString()}};
        QVERIFY(controller.writeCalendarMeeting("edit", id, draft));
        QTRY_VERIFY(!sent.isEmpty());
        QCOMPARE(sent.value("location"), original.value("location"));
        QTRY_VERIFY(!controller.calendarWriting() && !controller.calendarLoading());
        original["location"] = QJsonValue::Null;
        event["payload"] = original;
        controller.retryCalendar();
        QTRY_VERIFY(!controller.calendarLoading());
        draft["title"] = original.value("title").toString();
        draft["location"] = "";
        draft["endUtc"] = "2026-11-01T23:00:00Z";
        sent = {};
        QVERIFY(controller.writeCalendarMeeting("edit", id, draft));
        QTRY_VERIFY(!sent.isEmpty());
        QCOMPARE(sent.value("title"), original.value("title"));
        QVERIFY(sent.value("location").isNull());
    }

    void calendarWritesFenceReadOnlyAndForeignResources()
    {
        Fixture fixture;
        const QString foreign = "a0000000-0000-4000-8000-000000000001";
        const QString eventId = "a0000000-0000-4000-8000-000000000002";
        const auto initial = fixture.server.handler;
        int writes = 0, recoveries = 0;
        QJsonObject submitted;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/calendar/calendars/") {
                auto work = calendarList().first().toObject();
                work["read_only"] = false;
                work["external_account_uuid"] = QJsonValue::Null;
                auto frozen = calendarList().last().toObject();
                frozen["read_only"] = true;
                frozen["external_account_uuid"] = QJsonValue::Null;
                auto imported = work;
                imported["uuid"] = streamId;
                imported["external_account_uuid"] = topicId;
                return {QJsonArray{work, frozen, imported}};
            }
            if (!path.startsWith("/api/workspace/v1/calendar/events/"))
                return initial(request);
            if (request.method == "GET" && path.endsWith('/'))
                return {QJsonArray{calendarEvent(eventId, "2026-11-01", "09:00", "10:00", "Read-only", personalCalendarId)}};
            if (request.method == "GET") {
                ++recoveries;
                auto wrong = submitted;
                wrong["project_id"] = foreign;
                return {wrong};
            }
            ++writes;
            submitted = request.body;
            return {QJsonObject{}, 409};
        };
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_COMPARE(controller.calendarEvents().size(), 1);
        QVariantMap draft{{"calendar", workCalendarId}, {"title", "Test"}, {"startUtc", "2026-11-01T11:30:00Z"},
                          {"endUtc", "2026-11-01T12:00:00Z"}};
        for (const auto &id : {foreign, personalCalendarId, streamId}) {
            auto unavailable = draft;
            unavailable["calendar"] = id;
            QVERIFY(!controller.writeCalendarMeeting("create", topicId, unavailable));
        }
        QVERIFY(!controller.writeCalendarMeeting("move", eventId, draft));
        QVERIFY(!controller.writeCalendarMeeting("delete", eventId, {}));
        QVERIFY(!controller.writeCalendarMeeting("edit", foreign, draft));
        QCOMPARE(writes, 0);
        QSignalSpy finished(&controller, &NativeController::calendarWriteFinished);
        QVERIFY(controller.writeCalendarMeeting("create", topicId, draft));
        QVERIFY(!controller.writeCalendarMeeting("create", topicId, draft));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 4000);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(writes, 1);
        QCOMPARE(recoveries, 1);
        QCOMPARE(controller.calendarEvents().size(), 1);
        auto changed = draft;
        changed["title"] = "Another draft";
        QVERIFY(!controller.writeCalendarMeeting("create", topicId, changed));
        // Retrying the form retains its UUID even when recovery could not
        // confirm the first result.
        QVERIFY(controller.writeCalendarMeeting("create", topicId, draft));
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(submitted.value("uuid").toString(), topicId);
        QCOMPARE(writes, 2);
    }

    void calendarMutationFromPreviousUserCannotCompleteForNewUser()
    {
        Fixture fixture;
        QString user = userId;
        const auto fallback = fixture.server.handler;
        std::function<void()> release;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/actions/introspect"))
                return {QJsonObject{{"user_info", QJsonObject{{"uuid", user}}},
                                    {"project_id", projectId}, {"otp_verified", true}}};
            if (path == "/api/workspace/v1/calendar/calendars/") {
                auto row = calendarList().first().toObject();
                row["read_only"] = false;
                row["external_account_uuid"] = QJsonValue::Null;
                return {QJsonArray{row}};
            }
            if (path == "/api/workspace/v1/calendar/events/" && request.method == "POST") {
                auto answer = request.body;
                answer["project_id"] = projectId;
                Response response{answer};
                response.defer = [&](auto send) { release = std::move(send); };
                return response;
            }
            return fallback(request);
        };
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_VERIFY(!controller.calendarLoading());
        const QVariantMap draft{{"calendar", workCalendarId}, {"title", "Old owner"},
                                {"startUtc", "2026-11-01T11:30:00Z"}, {"endUtc", "2026-11-01T12:00:00Z"}};
        QVERIFY(controller.writeCalendarMeeting("create", topicId, draft));
        QTRY_VERIFY(release);
        controller.logout();
        QTRY_VERIFY(!controller.authenticated());
        user = streamId;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.session.user(), user);
        QVERIFY(!controller.calendarWriting());
        QSignalSpy finished(&controller, &NativeController::calendarWriteFinished);
        release();
        QTest::qWait(150);
        QVERIFY(finished.isEmpty());
        QVERIFY(controller.calendarEvents().isEmpty());
        QVERIFY(controller.calendarCatalog().isEmpty());
    }

    void groupwareCursorRefreshesVisibleCalendarAndDefersMailUntilEntry()
    {
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        QVector<QJsonObject> envelopes;
        fixture.realtime = [&](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows;
            const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : envelopes)
                if (event.value("epoch_version").toInt() > after) rows.append(event);
            return Response{rows};
        };
        int calendarReads = 0, mailReads = 0;
        bool messageExists = true;
        const QString eventId = "a2000000-0000-4000-8000-000000000001";
        fixture.calendar = [&](const Request &) {
            ++calendarReads;
            return Response{QJsonArray{calendarEvent(eventId, "2026-11-01", "11:00", "12:00",
                QString::number(calendarReads))}};
        };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/mail/folders/")
                return {QJsonArray{QJsonObject{{"uuid", topicId}, {"kind", "inbox"}, {"name", "Inbox"},
                                              {"project_id", projectId}}}};
            if (path == "/api/workspace/v1/mail/messages/") {
                ++mailReads;
                const QJsonObject row{{"uuid", messageId}, {"project_id", projectId}, {"folder_uuid", topicId},
                                      {"subject", QString::number(mailReads)}};
                return {messageExists ? QJsonArray{row} : QJsonArray{}};
            }
            return fallback(request);
        };
        QVERIFY(fixture.start(gateway.origin()));
        auto &controller = fixture.controller;
        RealtimeClient realtime(&fixture.api, &fixture.session);
        controller.useRealtime(&realtime);
        QTRY_COMPARE(controller.syncState(), "live");
        QTRY_COMPARE(gateway.sockets.size(), 1);
        auto *mail = controller.mail();
        controller.showSection("mail");
        mail->refresh();
        QTRY_COMPARE(mail->messages().size(), 1);
        mail->selectMessage(messageId);
        QSignalSpy ownerRetired(mail, &MailStore::ownerRetired);
        controller.showSection("calendar");
        controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_COMPARE(controller.calendarEvents().size(), 1);
        const auto send = [&](QJsonObject event) {
            envelopes.append(event);
            gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        };
        auto calendar = controllerEvent("calendar.event.updated", QJsonObject{{"uuid", eventId}}, 11);
        auto foreign = calendar;
        foreign["user_uuid"] = streamId;
        QVERIFY(!controller.applyRealtimeEvent(foreign));
        send(calendar);
        QTRY_COMPARE(calendarReads, 2);
        QTRY_COMPARE(controller.calendarEvents().first().toMap().value("title").toString(), QStringLiteral("2"));
        // A duplicate envelope UUID at a newer epoch is acknowledged without
        // repeating the HTTP read or blocking the shared cursor.
        calendar["epoch_version"] = 12;
        send(calendar);
        send(controllerEvent("mail.message.updated", QJsonObject{{"uuid", messageId}}, 13));
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(13));
        QTest::qWait(200);
        QCOMPARE(calendarReads, 2);
        QCOMPARE(mailReads, 1);
        controller.showSection("mail");
        QTRY_COMPARE(mailReads, 2);
        QTRY_COMPARE(mail->state(), QStringLiteral("ready"));
        QCOMPARE(mail->selectedMessageUuid(), messageId);
        QVERIFY(ownerRetired.isEmpty());
        // The second live handshake must pick up a missed deletion. It also
        // marks Calendar stale without fetching that hidden section.
        messageExists = false;
        gateway.sockets.last()->close();
        QTRY_VERIFY_WITH_TIMEOUT(gateway.sockets.size() >= 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(mailReads, 3, 5000);
        QTRY_VERIFY(mail->messages().isEmpty());
        QVERIFY(mail->selectedMessageUuid().isEmpty());
        QVERIFY(ownerRetired.isEmpty());
        QCOMPARE(calendarReads, 2);
        controller.showSection("calendar");
        QTRY_COMPARE(calendarReads, 3);
    }

    void groupwareEventDuringCalendarReadRefreshesAfterTheCurrentPage()
    {
        Fixture fixture;
        int reads = 0;
        std::function<void()> release;
        fixture.calendar = [&](const Request &) {
            ++reads;
            Response response{QJsonArray{}};
            if (reads == 1)
                response.defer = [&](auto send) { release = std::move(send); };
            return response;
        };
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        controller.showSection("calendar");
        controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_VERIFY(release);
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("calendar.calendar.updated", {{"uuid", workCalendarId}}, 42)));
        QTest::qWait(150);
        QCOMPARE(reads, 1);
        release();
        QTRY_COMPARE_WITH_TIMEOUT(reads, 2, 4000);
        QTRY_VERIFY(!controller.calendarLoading());
    }

    void calendarReadsOwnedPagedCollectionsWithLocalOverlap()
    {
        Fixture fixture;
        const auto fallback = fixture.server.handler;
        const auto firstDay = QDate(2026, 11, 1); // 25 hours in America/New_York
        const auto nextMidnight = firstDay.addDays(1).startOfDay().toUTC();
        const auto nextUtcSecond = nextMidnight.toString("yyyy-MM-dd'T'HH:mm:ss");
        const QString firstId = "92000000-0000-4000-8000-000000000001";
        const QString secondId = "92000000-0000-4000-8000-000000000002";
        const QString otherId = "92000000-0000-4000-8000-000000000003";
        const auto event = [&](const QString &id, const QString &end) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"calendar_uuid", personalCalendarId},
                {"payload", QJsonObject{{"kind", "event"}, {"title", "Night shift"}, {"location", "Room 4"},
                    {"starts_at", nextMidnight.addSecs(-3600).toString(Qt::ISODate)}, {"ends_at", end}}}};
        };
        const auto exclusiveEnd = event(firstId, nextUtcSecond + ".000000Z");
        const auto inclusiveEnd = event(secondId, nextUtcSecond + ".000001Z");
        QVector<Request> seen;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (!path.startsWith("/api/workspace/v1/calendar/"))
                return fallback(request);
            seen.append(request);
            const QUrlQuery query(request.target);
            Response response{QJsonArray{}};
            if (path.endsWith("/calendars/")) {
                const auto catalog = calendarList();
                if (!query.hasQueryItem("page_marker")) {
                    auto foreign = catalog.first().toObject();
                    foreign["uuid"] = otherId;
                    foreign["project_id"] = streamId;
                    response.data = QJsonArray{catalog.first(), foreign};
                    response.headers.insert("X-Pagination-Marker", "next");
                } else {
                    response.data = QJsonArray{catalog.last()};
                }
            } else if (path.endsWith("/events/")) {
                if (!query.hasQueryItem("page_marker")) {
                    response.data = QJsonArray{exclusiveEnd};
                    response.headers.insert("X-Pagination-Marker", "next");
                } else {
                    auto foreign = event(otherId, nextUtcSecond + "Z");
                    foreign["project_id"] = streamId;
                    auto unowned = event(topicId, nextUtcSecond + "Z");
                    unowned["calendar_uuid"] = otherId;
                    response.data = QJsonArray{exclusiveEnd, inclusiveEnd, foreign, unowned};
                }
            }
            return response;
        };
        QVERIFY(fixture.start());
        const auto day = firstDay.toString(Qt::ISODate);
        fixture.controller.requestCalendarWindow(day, day);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.calendarEvents().size(), 2, 4000);
        QCOMPARE(seen.size(), 4);
        for (int i = 0; i < seen.size(); ++i) {
            const auto &request = seen.at(i);
            const auto eventPage = i >= 2;
            QCOMPARE(request.method, QByteArray("GET"));
            QCOMPARE(request.target.path(), eventPage ? "/api/workspace/v1/calendar/events/"
                                                      : "/api/workspace/v1/calendar/calendars/");
            const QUrlQuery query(request.target);
            QCOMPARE(query.queryItemValue("page_limit"), QStringLiteral("200"));
            QCOMPARE(query.queryItemValue("page_marker"), i % 2 ? QStringLiteral("next") : QString{});
            QVERIFY(!query.hasQueryItem("project_id"));
            QVERIFY(!query.hasQueryItem("user_uuid"));
            QCOMPARE(query.hasQueryItem("ends_after"), eventPage);
            QCOMPARE(query.hasQueryItem("starts_before"), eventPage);
            if (eventPage) {
                QCOMPARE(query.queryItemValue("ends_after"), firstDay.startOfDay().toUTC().toString(Qt::ISODate));
                QCOMPARE(query.queryItemValue("starts_before"), nextUtcSecond + "Z");
            }
        }
        const auto calendars = fixture.controller.property("calendarCatalog").toList();
        QCOMPARE(calendars.size(), 2);
        QCOMPARE(calendars.last().toMap().value("label").toString(), QStringLiteral("On call"));
        const auto rows = fixture.controller.calendarEvents();
        QCOMPARE(rows.first().toMap().value("lastDay").toString(), day);
        const auto spill = rows.last().toMap();
        QCOMPARE(spill.value("day").toString(), day);
        QCOMPARE(spill.value("lastDay").toString(), firstDay.addDays(1).toString(Qt::ISODate));
        QCOMPARE(spill.value("calendar").toString(), personalCalendarId);
        QCOMPARE(spill.value("endUtc").toString(), nextUtcSecond + ".000001Z");
        QCOMPARE(spill.value("location").toString(), QStringLiteral("Room 4"));
        QVERIFY(spill.value("participants").toList().isEmpty());
    }

    void calendarSwitchingUserRetiresDelayedOwnedRows()
    {
        Fixture fixture;
        QString loginUser = userId;
        fixture.authentication = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/introspect"))
                return Response{QJsonObject{{"user_info", QJsonObject{{"uuid", loginUser}}},
                                            {"project_id", projectId}, {"otp_verified", true}}};
            return sessionResponse(request);
        };
        std::function<void()> oldResponse;
        fixture.calendar = [&](const Request &) {
            const auto title = loginUser == userId ? QStringLiteral("First owner's meeting")
                                                   : QStringLiteral("Second owner's meeting");
            Response response{QJsonArray{calendarEvent("93000000-0000-4000-8000-000000000001",
                "2026-11-01", "09:00", "10:00", title)}};
            if (loginUser == userId)
                response.defer = [&](auto send) { oldResponse = std::move(send); };
            return response;
        };
        QVERIFY(fixture.start());
        fixture.controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_VERIFY_WITH_TIMEOUT(oldResponse, 3000);
        fixture.controller.logout();
        QTRY_VERIFY(!fixture.controller.authenticated());
        loginUser = streamId;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.session.user(), loginUser);
        QVERIFY(fixture.controller.calendarEvents().isEmpty());
        QVERIFY(fixture.controller.property("calendarCatalog").toList().isEmpty());
        fixture.controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.calendarEvents().size(), 1, 4000);
        oldResponse();
        QTest::qWait(150);
        QCOMPARE(fixture.controller.calendarEvents().first().toMap().value("title").toString(),
                 QStringLiteral("Second owner's meeting"));
    }

    void calendarInterruptedPaginationNeverPublishesPartialData_data()
    {
        QTest::addColumn<bool>("cycle");
        QTest::newRow("forbidden") << false;
        QTest::newRow("repeated-marker") << true;
    }

    void calendarInterruptedPaginationNeverPublishesPartialData()
    {
        QFETCH(bool, cycle);
        Fixture fixture;
        int pages = 0;
        bool recovering = false;
        const auto row = calendarEvent("94000000-0000-4000-8000-000000000001", "2026-11-01",
                                      "09:00", "10:00", "First page");
        fixture.calendar = [&](const Request &request) {
            ++pages;
            const bool more = QUrlQuery(request.target).hasQueryItem("page_marker");
            Response response{QJsonArray{row}};
            if (!recovering) {
                if (more && !cycle)
                    return Response{QJsonObject{}, 403};
                response.headers.insert("X-Pagination-Marker", "same");
            }
            return response;
        };
        QVERIFY(fixture.start());
        fixture.controller.requestCalendarWindow("2026-11-01", "2026-11-01");
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.calendarError(), 4000);
        QCOMPARE(pages, 2);
        QVERIFY(!fixture.controller.calendarLoading());
        QVERIFY(fixture.controller.calendarEvents().isEmpty());
        QVERIFY(fixture.controller.property("calendarCatalog").toList().isEmpty());
        recovering = true;
        fixture.controller.retryCalendar();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.calendarEvents().size(), 1, 4000);
        QCOMPARE(pages, 3);
        QVERIFY(!fixture.controller.calendarError());
    }

    // Issue #926: a same-account token renewal bumps the API generation and
    // aborts the in-flight GET /calendar/events; the shared request wrapper
    // drops the retired callback, so nothing ever cleared the calendar
    // inflight/loading flags and every later entry point (date navigation,
    // reopen, retry) refused to load for the rest of the session. The renewal
    // must settle the interrupted window into an actionable error state, a
    // retry must dispatch a fresh GET under the renewed session, and the
    // retired response must not settle or replace the newer request.
    void calendarSameOwnerRenewalSettlesInterruptedLoadAndRetryReloads()
    {
        Fixture fixture;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto calendarGets = std::make_shared<int>(0);
        fixture.calendar = [gates, calendarGets](const Request &) {
            ++*calendarGets;
            Response response{QJsonArray{calendarEvent("91000000-0000-4000-8000-000000000001",
                "2026-07-02", "09:00", "10:00", "Standup")}};
            response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
            return response;
        };
        QVERIFY(fixture.start());
        fixture.controller.requestCalendarWindow(
            QStringLiteral("2026-07-01"), QStringLiteral("2026-07-07"));
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first window is in flight
        QVERIFY(fixture.controller.calendarLoading());
        const int firstWindowGets = *calendarGets;
        fixture.api.setAccessToken("synthetic-renewed-access-token");
        // AC1: the interrupted fetch settles its transient state into the
        // actionable error the page already knows how to render.
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.calendarLoading(), 4000);
        QVERIFY(fixture.controller.calendarError());
        // AC2: a retry under the renewed session dispatches a fresh GET.
        fixture.controller.retryCalendar();
        QTRY_VERIFY_WITH_TIMEOUT(*calendarGets >= firstWindowGets + 1, 4000);
        QVERIFY(fixture.controller.calendarLoading());
        QCOMPARE(*calendarGets, firstWindowGets + 1);
        // AC3: the retired first-window answer may not settle the newer request.
        (*gates)[0]();
        QTest::qWait(250); // give the retired callback a chance to misfire
        QVERIFY(fixture.controller.calendarLoading());
        QVERIFY(fixture.controller.calendarEvents().isEmpty());
        // The newer request's own answer settles it and the page stays usable.
        QVERIFY(gates->size() >= 2);
        (*gates)[1]();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.calendarEvents().size() == 1, 4000);
        QVERIFY(!fixture.controller.calendarLoading());
        QVERIFY(!fixture.controller.calendarError());
    }
};

QTEST_MAIN(ControllerInboxTest)
#include "native-controller-inbox-test.moc"
