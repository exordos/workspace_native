#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"
#include "support/controller-keys-fixtures.h"

class ControllerOutboxTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // This child is a real second process with the production drafts/storage
    // and controller. Only HTTP transport uses the parent's local fixture.
    void outgoingRetryReconcilesBeforeReplayingTheImmutableBody_data()
    {
        QTest::addColumn<int>("readStatus");
        QTest::newRow("authoritative-absence") << 404;
        QTest::newRow("network-failure") << 503;
        QTest::newRow("access-denied") << 403;
        QTest::newRow("foreign-body") << 200;
        QTest::newRow("foreign-viewer") << 201;
    }
    void outgoingRetryReconcilesBeforeReplayingTheImmutableBody()
    {
        QFETCH(int, readStatus);
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        QVector<QJsonObject> posts; QStringList reads; QJsonObject saved;
        bool durableAtPost = false;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                posts.append(request.body);
                const auto connection = "cassi-outgoing-observe-commit";
                {
                    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                    db.setDatabaseName(directory.filePath(drafts.owner() + ".sqlite3"));
                    if (db.open()) {
                        QSqlQuery query(db); query.prepare("SELECT uuid FROM outgoing_messages WHERE uuid=?");
                        query.addBindValue(request.body.value("uuid").toString());
                        durableAtPost = query.exec() && query.next(); db.close();
                    }
                }
                QSqlDatabase::removeDatabase(connection);
                const auto placement = QUuid::createUuidV5(QUuid(topicId), request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved = message(request.body.value("payload").toObject().value("content").toString(), placement);
                return posts.size() == 1 ? Response{QJsonObject{}, 503} : Response{saved};
            }
            if (!request.target.path().endsWith('/')) {
                reads.append(request.target.path().section('/', -1));
                if (posts.size() > 1) return Response{saved};
                if (readStatus == 201) { auto foreign = saved; foreign["user_uuid"] = secondTopicId; return Response{foreign}; }
                return readStatus == 200 ? Response{message("different canonical body", saved.value("uuid").toString())}
                    : Response{QJsonObject{}, readStatus};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Original exact body"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_COMPARE(posts.size(), 1); QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(durableAtPost);
        QVERIFY(!fixture.controller.outgoingSend().isEmpty()); QVERIFY(!fixture.controller.canSend());
        fixture.controller.setDraft("Newer editor text");
        fixture.controller.retryPendingSend(); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(reads.first(), saved.value("uuid").toString());
        QCOMPARE(posts.size(), readStatus == 404 ? 2 : 1);
        if (readStatus == 404) { QCOMPARE(posts[0], posts[1]); QVERIFY(fixture.controller.outgoingSend().isEmpty()); }
        else {
            QVERIFY(!fixture.controller.outgoingSend().isEmpty());
            fixture.controller.discardPendingSend(); QTRY_VERIFY(fixture.controller.outgoingSend().isEmpty());
        }
        QCOMPARE(fixture.controller.draft(), "Newer editor text");
    }

    void outgoingStorageFailureNeverDispatchesAndRetriesTheSameIntent()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts); int posts = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { ++posts; return Response{QJsonObject{}, 503}; }
            return request.target.path().endsWith('/') ? Response{QJsonArray{}} : Response{QJsonObject{}, 404};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Must commit before POST"); QTRY_VERIFY(fixture.controller.canSend());
        const auto connection = "cassi-outgoing-reject-write";
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(directory.filePath(drafts.owner() + ".sqlite3"));
        QVERIFY(db.open());
        {
            QSqlQuery query(db); QVERIFY(query.exec("CREATE TRIGGER reject_outgoing BEFORE INSERT ON outgoing_messages BEGIN SELECT RAISE(ABORT, 'synthetic write failure'); END"));
        }
        fixture.controller.send(); QTRY_VERIFY(!fixture.controller.error().isEmpty()); QCOMPARE(posts, 0);
        const auto id = fixture.controller.outgoingSend().value("id"); QVERIFY(!id.toString().isEmpty());
        QCOMPARE(fixture.controller.draft(), "Must commit before POST");
        { QSqlQuery query(db); QVERIFY(query.exec("DROP TRIGGER reject_outgoing")); }
        db.close(); db = {}; QSqlDatabase::removeDatabase(connection);
        fixture.controller.retryPendingSend(); QTRY_COMPARE(posts, 1); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.outgoingSend().value("id"), id);
    }

    void outgoingRestorationWaitsForItsOwnerAndDoesNotReplayAfterLogout()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts); bool other = false; int posts = 0, gets = 0;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (other && request.target.path().endsWith("/actions/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", QJsonObject{{"uuid", secondTopicId}}}};
            return response;
        };
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { ++posts; return Response{QJsonObject{}, 503}; }
            if (!request.target.path().endsWith('/')) { ++gets; return Response{QJsonObject{}, 404}; }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Owner A pending"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_COMPARE(posts, 1); QTRY_VERIFY(!fixture.controller.busy());
        const auto id = fixture.controller.outgoingSend().value("id");
        fixture.controller.logout(); other = true; QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QVERIFY(fixture.controller.outgoingSend().isEmpty()); fixture.controller.retryPendingSend(); QCOMPARE(posts, 1); QCOMPARE(gets, 0);
        fixture.controller.logout(); other = false; QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QTRY_COMPARE(fixture.controller.outgoingSend().value("id"), id); QTRY_COMPARE(gets, 1);
        QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(posts, 1);
        fixture.controller.retryPendingSend(); QTRY_COMPARE(posts, 2); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.outgoingSend().value("id"), id);
    }

    void outgoingForgetRequiresCommittedCleanupBeforeRemovingTheCredential()
    {
        QTemporaryDir directory; ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture; LoopbackKeyTls tls(fixture.server.serverPort()); fixture.session.setVault(&vault); fixture.controller.useDrafts(&drafts);
        fixture.messages = [](const Request &request) {
            if (request.method == "POST") return Response{QJsonObject{}, 503};
            return request.target.path().endsWith('/') ? Response{QJsonArray{}} : Response{QJsonObject{}, 404};
        };
        QVERIFY(fixture.start(tls.origin())); QTRY_VERIFY(drafts.ready(streamId, topicId));
        const auto server = fixture.api.server();
        SavedAccount account{SessionVault::accountId(server, projectId, userId), server, projectId, userId, "cassi", "Outgoing"};
        bool saved = false; vault.save(account, "synthetic-refresh", [&](const QString &error) { saved = error.isEmpty(); }); QTRY_VERIFY(saved);
        fixture.controller.setDraft("Pending account cleanup"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(!fixture.controller.outgoingSend().isEmpty());
        const auto connection = "cassi-outgoing-reject-forget";
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(directory.filePath("drafts/" + drafts.owner() + ".sqlite3")); QVERIFY(db.open());
        { QSqlQuery query(db); QVERIFY(query.exec("CREATE TRIGGER reject_forget BEFORE DELETE ON outgoing_messages BEGIN SELECT RAISE(ABORT, 'synthetic delete failure'); END")); }
        fixture.controller.forgetSavedAccount(account.id); QTRY_VERIFY(fixture.controller.error().contains("saved account was kept"));
        QVERIFY(secrets.values.contains(account.id)); QCOMPARE(fixture.controller.savedAccounts().size(), 1);
        { QSqlQuery query(db); QVERIFY(query.exec("DROP TRIGGER reject_forget")); }
        fixture.controller.forgetSavedAccount(account.id); QTRY_VERIFY(!secrets.values.contains(account.id));
        QCOMPARE(fixture.controller.savedAccounts().size(), 0);
        { QSqlQuery query(db); QVERIFY(query.exec("SELECT COUNT(*) FROM outgoing_messages")); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(), 0); }
        db.close(); db = {}; QSqlDatabase::removeDatabase(connection);
    }

    void outgoingAccessLossHidesTheQueueAndPreventsReplay()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); int posts = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { ++posts; return Response{QJsonObject{}, 503}; }
            return request.target.path().endsWith('/') ? Response{QJsonArray{}} : Response{QJsonObject{}, 404};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Pending before access loss"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_COMPARE(posts, 1); QTRY_VERIFY(!fixture.controller.busy());
        fixture.streams = [](const Request &) { return Response{QJsonArray{}}; }; fixture.controller.refresh();
        QTRY_VERIFY(fixture.controller.streams().isEmpty()); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(fixture.controller.outgoingSend().isEmpty()); fixture.controller.retryPendingSend(); fixture.controller.send(); QCOMPARE(posts, 1);
    }

    void outgoingDiscardKeepsRetryBlockedUntilItsDurableReceipt()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        NativeCacheService cache(directory.filePath("cache"), {}, true);
        Fixture fixture; fixture.controller.useDrafts(&drafts); fixture.controller.useCache(&cache); int posts = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") { ++posts; return Response{QJsonObject{}, 503}; }
            return request.target.path().endsWith('/') ? Response{QJsonArray{}} : Response{QJsonObject{}, 404};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Keep discard receipt owned"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_COMPARE(posts, 1); QTRY_VERIFY(!fixture.controller.busy());
        const auto connection = "cassi-outgoing-hold-discard";
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(directory.filePath("drafts/" + drafts.owner() + ".sqlite3")); QVERIFY(db.open());
        auto release = qScopeGuard([&] { { QSqlQuery query(db); query.exec("ROLLBACK"); } db.close(); db = {}; QSqlDatabase::removeDatabase(connection); });
        { QSqlQuery query(db); QVERIFY(query.exec("BEGIN IMMEDIATE")); }
        fixture.controller.discardPendingSend(); fixture.controller.useCache(nullptr);
        QTRY_VERIFY(!fixture.controller.outgoingSend().isEmpty());
        QVERIFY(fixture.controller.outgoingSend().value("busy").toBool());
        fixture.controller.retryPendingSend(); QCOMPARE(posts, 1);
        { QSqlQuery query(db); QVERIFY(query.exec("COMMIT")); }
        QTRY_VERIFY(fixture.controller.outgoingSend().isEmpty()); QCOMPARE(posts, 1);
        QCOMPARE(fixture.controller.draft(), "Keep discard receipt owned");
    }

    void encryptedOutgoingCannotRetryWithoutDurableAdmission()
    {
        QTemporaryDir directory; KeyManagementFixture fixture;
        NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage); fixture.controller.useDrafts(&drafts);
        int posts = 0; const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) {
            if (request.method == "POST" && request.body.value("payload").toObject().value("kind") == QJsonValue("e2ee")) {
                ++posts; return Response{QJsonObject{}, 503};
            }
            return fallback(request);
        };
        QVERIFY(fixture.start()); fixture.seedKey(KeyManagementFixture::KeyCurrent); fixture.controller.refreshChatLocalKeys();
        fixture.controller.setDraft("Encrypted admission must precede every POST"); QTRY_VERIFY(fixture.controller.canSend());
        const auto connection = "cassi-outgoing-reject-encrypted";
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(directory.filePath(fixture.controller.creationOwnerKey() + ".sqlite3")); QVERIFY(db.open());
        { QSqlQuery query(db); QVERIFY(query.exec("CREATE TRIGGER reject_outgoing BEFORE INSERT ON outgoing_messages BEGIN SELECT RAISE(ABORT, 'synthetic write failure'); END")); }
        fixture.controller.send(); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(posts, 0);
        const auto id = fixture.controller.outgoingSend().value("id"); QVERIFY(!id.toString().isEmpty());
        fixture.controller.retryPendingSend(); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(posts, 0);
        { QSqlQuery query(db); QVERIFY(query.exec("DROP TRIGGER reject_outgoing")); }
        db.close(); db = {}; QSqlDatabase::removeDatabase(connection);
        fixture.controller.retryPendingSend(); QTRY_COMPARE(posts, 1); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.outgoingSend().value("id"), id);
    }

    void encryptedOutgoingDiscardFailureKeepsTheIntentRetryable()
    {
        QTemporaryDir directory; KeyManagementFixture fixture;
        NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage); fixture.controller.useDrafts(&drafts);
        int posts = 0; const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) {
            if (request.method == "POST" && request.body.value("payload").toObject().value("kind") == QJsonValue("e2ee")) {
                ++posts; return Response{QJsonObject{}, 503};
            }
            return fallback(request);
        };
        QVERIFY(fixture.start()); fixture.seedKey(KeyManagementFixture::KeyCurrent); fixture.controller.refreshChatLocalKeys();
        fixture.controller.setDraft("Encrypted discard failure"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_COMPARE(posts, 1); QTRY_VERIFY(!fixture.controller.busy());
        const auto connection = "cassi-outgoing-reject-encrypted-discard";
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(directory.filePath(fixture.controller.creationOwnerKey() + ".sqlite3")); QVERIFY(db.open());
        auto release = qScopeGuard([&] { db.close(); db = {}; QSqlDatabase::removeDatabase(connection); });
        { QSqlQuery query(db); QVERIFY(query.exec("CREATE TRIGGER reject_discard BEFORE DELETE ON outgoing_messages BEGIN SELECT RAISE(ABORT, 'synthetic delete failure'); END")); }
        fixture.controller.discardPendingSend();
        QTRY_VERIFY(fixture.controller.outgoingSend().value("error").toString().contains("could not be removed"));
        QVERIFY(!fixture.controller.busy()); QVERIFY(fixture.controller.outgoingSend().value("canRetry").toBool());
        { QSqlQuery query(db); QVERIFY(query.exec("DROP TRIGGER reject_discard")); }
        fixture.controller.discardPendingSend();
        QCOMPARE(fixture.vault.remove(fixture.vault.lease(), {streamId, KeyManagementFixture::KeyCurrent}), NativeChatKeyVault::Failure::None);
        QTRY_VERIFY(fixture.controller.outgoingSend().isEmpty());
        QVERIFY(!fixture.controller.busy()); QCOMPARE(posts, 1);
    }

    void outgoingDeletedCanonicalDoesNotConsumeItsDraft()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        QJsonObject saved; std::function<void()> confirm;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                const auto placement = QUuid::createUuidV5(QUuid(topicId), request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved = message(request.body.value("payload").toObject().value("content").toString(), placement); return Response{saved};
            }
            if (!request.target.path().endsWith('/')) {
                Response response{saved}; response.defer = [&](auto send) { confirm = std::move(send); }; return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Keep deleted send text"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_VERIFY(bool(confirm));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{
            {"uuid", saved.value("uuid")}, {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}})));
        confirm(); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.draft(), "Keep deleted send text");
        bool flushed = false; drafts.flush(this, [&](bool okay) { flushed = okay; }); QTRY_VERIFY(flushed);
        const auto owner = drafts.owner(); fixture.controller.useDrafts(nullptr); storage.shutdown(); QTRY_VERIFY(storage.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(owner), NativeDraftStore::Failure::None);
        QVERIFY(store.latest(streamId, topicId).record); QCOMPARE(store.outgoing().rows.size(), 1);
    }

    void outgoingDeletionDuringRetirementPreservesTheRestartRecord()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        QJsonObject saved; std::function<void()> confirm;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                const auto placement = QUuid::createUuidV5(QUuid(topicId), request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved = message(request.body.value("payload").toObject().value("content").toString(), placement); return Response{saved};
            }
            if (!request.target.path().endsWith('/')) {
                Response response{saved}; response.defer = [&](auto send) { confirm = std::move(send); }; return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Keep text deleted during retirement");
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send(); QTRY_VERIFY(bool(confirm));
        QTRY_VERIFY(!storage.busy());
        const auto connection = "cassi-outgoing-hold-retirement";
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(directory.filePath(drafts.owner() + ".sqlite3")); QVERIFY(db.open());
        auto release = qScopeGuard([&] { { QSqlQuery query(db); query.exec("ROLLBACK"); } db.close(); db = {}; QSqlDatabase::removeDatabase(connection); });
        { QSqlQuery query(db); QVERIFY(query.exec("BEGIN IMMEDIATE")); }
        // A real queued worker write holds retirement until the main thread has
        // observed the deletion. The removed UUID is deliberately absent.
        bool precedingWrite = false;
        QVERIFY(storage.apply(storage.lease(), {}, {secondTopicId}, this,
            [&](auto result) { precedingWrite = result == NativeDraftService::Failure::None; }));
        confirm(); QTRY_COMPARE(storage.pendingDataJobs(), 2);
        QElapsedTimer observation; observation.start();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{
            {"uuid", saved.value("uuid")}, {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}})));
        QVERIFY2(observation.elapsed() < 100, "Deletion must not wait on the worker's held SQLite writer");
        { QSqlQuery query(db); QVERIFY(query.exec("COMMIT")); }
        QTRY_VERIFY(precedingWrite); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.draft(), "Keep text deleted during retirement");
        bool flushed = false; drafts.flush(this, [&](bool okay) { flushed = okay; }); QTRY_VERIFY(flushed);
        const auto owner = drafts.owner(); fixture.controller.useDrafts(nullptr); storage.shutdown(); QTRY_VERIFY(storage.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(owner), NativeDraftStore::Failure::None);
        const auto restored = store.latest(streamId, topicId);
        QVERIFY(restored.record); QVERIFY(!restored.record->consumed);
        QCOMPARE(restored.record->text, "Keep text deleted during retirement");
        QCOMPARE(store.outgoing().rows.size(), 1);
    }

    void encryptedOutgoingDeletionDuringRetirementKeepsTheCiphertextRecord()
    {
        QTemporaryDir directory; KeyManagementFixture fixture;
        NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage); fixture.controller.useDrafts(&drafts);
        QJsonObject saved; std::function<void()> confirm; const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) {
            if (request.method == "POST" && request.body.value("payload").toObject().value("kind") == QJsonValue("e2ee")) {
                const auto topic = request.body.value("topic_uuid").toString();
                const auto placement = QUuid::createUuidV5(QUuid(topic), request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved = message({}, placement, topic); saved["payload"] = request.body.value("payload"); return Response{saved};
            }
            if (!saved.isEmpty() && request.target.path().endsWith(saved.value("uuid").toString())) {
                Response response{saved}; response.defer = [&](auto send) { confirm = std::move(send); }; return response;
            }
            return fallback(request);
        };
        QVERIFY(fixture.start()); fixture.seedKey(KeyManagementFixture::KeyCurrent); fixture.controller.refreshChatLocalKeys();
        fixture.controller.setDraft("Private text deleted during retirement");
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send(); QTRY_VERIFY(bool(confirm)); QTRY_VERIFY(!storage.busy());
        const auto connection = "cassi-encrypted-outgoing-hold-retirement";
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(directory.filePath(fixture.controller.creationOwnerKey() + ".sqlite3")); QVERIFY(db.open());
        auto release = qScopeGuard([&] { { QSqlQuery query(db); query.exec("ROLLBACK"); } db.close(); db = {}; QSqlDatabase::removeDatabase(connection); });
        { QSqlQuery query(db); QVERIFY(query.exec("BEGIN IMMEDIATE")); }
        bool precedingWrite = false;
        QVERIFY(storage.apply(storage.lease(), {}, {secondTopicId}, this,
            [&](auto result) { precedingWrite = result == NativeDraftService::Failure::None; }));
        confirm(); QTRY_COMPARE(storage.pendingDataJobs(), 2);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{
            {"uuid", saved.value("uuid")}, {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", saved.value("topic_uuid")}})));
        { QSqlQuery query(db); QVERIFY(query.exec("COMMIT")); }
        QTRY_VERIFY(precedingWrite); QTRY_VERIFY(!storage.busy());
        const auto owner = fixture.controller.creationOwnerKey(); fixture.controller.useDrafts(nullptr);
        storage.shutdown(); QTRY_VERIFY(storage.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(owner), NativeDraftStore::Failure::None);
        const auto rows = store.outgoing().rows; QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].payload, saved.value("payload").toObject());
        QVERIFY(rows[0].draftText.isEmpty()); QVERIFY(rows[0].draftUuid.isEmpty());
        QVERIFY(!fixture.controller.busy());
    }

    void outgoingDeletionAfterRetirementDoesNotResurrectOrStrandTheSend()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); QJsonObject saved; bool deleted = false;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                const auto placement = QUuid::createUuidV5(QUuid(topicId), request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved = message(request.body.value("payload").toObject().value("content").toString(), placement); return Response{saved};
            }
            if (!request.target.path().endsWith('/')) return Response{saved};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        connect(&storage, &NativeDraftService::stateChanged, &fixture.controller, [&] {
            if (saved.isEmpty() || deleted || storage.busy()) return;
            // The worker has committed and emits its state before delivering the
            // controller receipt. Observe the actual database, not a test hook.
            const auto connection = "cassi-outgoing-observe-retired"; bool retired = false;
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                db.setDatabaseName(directory.filePath(drafts.owner() + ".sqlite3"));
                if (db.open()) { QSqlQuery query(db); retired = query.exec("SELECT count(*) FROM outgoing_messages") && query.next() && query.value(0).toInt() == 0; }
            }
            QSqlDatabase::removeDatabase(connection);
            if (!retired) return;
            deleted = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{
                {"uuid", saved.value("uuid")}, {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}})));
        });
        fixture.controller.setDraft("Confirmed before deletion"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_VERIFY(deleted); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(fixture.controller.outgoingSend().isEmpty()); QVERIFY(fixture.controller.draft().isEmpty());
        for (const auto &row : fixture.controller.messages()) QVERIFY(row.toMap().value("id").toString() != saved.value("uuid").toString());
    }

    void encryptedOutgoingDeletionAfterRetirementDoesNotResurrectOrStrandTheSend()
    {
        QTemporaryDir directory; KeyManagementFixture fixture;
        NativeDraftService storage(directory.path()); NativeDrafts drafts(&storage);
        fixture.controller.useDrafts(&drafts); QJsonObject saved; bool deleted = false;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) {
            if (request.method == "POST" && request.body.value("payload").toObject().value("kind") == QJsonValue("e2ee")) {
                const auto topic = request.body.value("topic_uuid").toString();
                const auto placement = QUuid::createUuidV5(QUuid(topic), request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved = message({}, placement, topic); saved["payload"] = request.body.value("payload"); return Response{saved};
            }
            if (!saved.isEmpty() && request.target.path().endsWith(saved.value("uuid").toString())) return Response{saved};
            return fallback(request);
        };
        QVERIFY(fixture.start()); fixture.seedKey(KeyManagementFixture::KeyCurrent); fixture.controller.refreshChatLocalKeys();
        connect(&storage, &NativeDraftService::stateChanged, &fixture.controller, [&] {
            if (saved.isEmpty() || deleted || storage.busy()) return;
            // The worker has committed and emits its state before delivering the
            // controller receipt. Observe the actual database, not a test hook.
            const auto connection = "cassi-outgoing-observe-retired"; bool retired = false;
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                db.setDatabaseName(directory.filePath(fixture.controller.creationOwnerKey() + ".sqlite3"));
                if (db.open()) { QSqlQuery query(db); retired = query.exec("SELECT count(*) FROM outgoing_messages") && query.next() && query.value(0).toInt() == 0; }
            }
            QSqlDatabase::removeDatabase(connection);
            if (!retired) return;
            deleted = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{
                {"uuid", saved.value("uuid")}, {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", saved.value("topic_uuid")}})));
        });
        fixture.controller.setDraft("Confirmed before deletion"); QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send();
        QTRY_VERIFY(deleted); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(fixture.controller.outgoingSend().isEmpty()); QVERIFY(fixture.controller.draft().isEmpty());
        for (const auto &row : fixture.controller.messages()) QVERIFY(row.toMap().value("id").toString() != saved.value("uuid").toString());
    }

    void encryptedOutgoingRestartsWithoutPlaintextAndWaitsForItsOriginalKey_data()
    {
        QTest::addColumn<bool>("deleteKey");
        QTest::newRow("original-key") << false;
        QTest::newRow("deleted-original-key") << true;
    }
    void encryptedOutgoingRestartsWithoutPlaintextAndWaitsForItsOriginalKey()
    {
        QFETCH(bool, deleteKey);
        QTemporaryDir directory; KeyManagementFixture fixture;
        auto storage = std::make_unique<NativeDraftService>(directory.path());
        auto drafts = std::make_unique<NativeDrafts>(storage.get()); fixture.controller.useDrafts(drafts.get());
        QVector<QJsonObject> posts; const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) {
            if (request.method == "POST" && request.body.value("payload").toObject().value("kind") == QJsonValue("e2ee")) {
                posts.append(request.body); return Response{QJsonObject{}, 503};
            }
            return fallback(request);
        };
        QVERIFY(fixture.start()); fixture.seedKey(KeyManagementFixture::KeyCurrent); fixture.controller.refreshChatLocalKeys();
        fixture.controller.setDraft("Private text must never enter the outgoing database");
        QTRY_VERIFY(fixture.controller.canSend()); fixture.controller.send(); QTRY_COMPARE(posts.size(), 1);
        QTRY_VERIFY(!fixture.controller.busy()); const auto original = posts[0];
        const auto owner = fixture.controller.creationOwnerKey();
        fixture.controller.useDrafts(nullptr); drafts.reset(); storage.reset();
        {
            NativeDraftStore store(directory.path()); QCOMPARE(store.open(owner), NativeDraftStore::Failure::None);
            QCOMPARE(store.outgoing().rows.size(), 1); const auto row = store.outgoing().rows[0];
            QVERIFY(row.draftText.isEmpty()); QVERIFY(row.draftUuid.isEmpty()); QVERIFY(row.replySnapshot.isEmpty());
            QCOMPARE(row.payload, original.value("payload").toObject());
        }
        if (deleteKey) QCOMPARE(fixture.vault.remove(fixture.vault.lease(), {streamId, KeyManagementFixture::KeyCurrent}), NativeChatKeyVault::Failure::None);
        NativeDraftService restoredStorage(directory.path()); NativeDrafts restoredDrafts(&restoredStorage);
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session); controller.useDrafts(&restoredDrafts);
        controller.login(fixture.tls.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId); QTRY_COMPARE(controller.streams().size(), 1);
        controller.selectStream(streamId); QTRY_VERIFY(!controller.topics().isEmpty()); controller.selectTopic(topicId);
        QTRY_VERIFY(!controller.outgoingSend().value("id").toString().isEmpty());
        QTRY_VERIFY(!controller.busy()); QVERIFY(!controller.outgoingSend().value("canRetry").toBool());
        QCOMPARE(posts.size(), 1); QVERIFY(controller.draft().isEmpty());
        controller.useChatKeys(&fixture.vault); controller.refreshChatLocalKeys();
        controller.retryPendingSend();
        if (deleteKey) {
            QTRY_VERIFY(controller.outgoingSend().value("error").toString().contains("original encryption key")); QCOMPARE(posts.size(), 1);
        } else {
            QTRY_COMPARE(posts.size(), 2); QCOMPARE(posts[0], posts[1]); QTRY_VERIFY(!controller.busy());
        }
        QVERIFY(controller.draft().isEmpty());
    }

    void outgoingRestartProcessChild()
    {
        const auto phase = qEnvironmentVariable("CASSI_OUTGOING_CHILD");
        if (phase.isEmpty()) return;
        NativeDraftService storage(qEnvironmentVariable("CASSI_OUTGOING_DIRECTORY"));
        NativeDrafts drafts(&storage);
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        controller.useDrafts(&drafts);
        controller.login(qEnvironmentVariable("CASSI_OUTGOING_ORIGIN"), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId); QTRY_COMPARE(controller.streams().size(), 1);
        controller.selectStream(streamId); QTRY_VERIFY(!controller.topics().isEmpty());
        controller.selectTopic(topicId); QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        if (phase == "admit") {
            controller.setDraft("Keep one message through a lost acknowledgement");
            QTRY_VERIFY(controller.canSend()); controller.send(); QTRY_VERIFY(!controller.error().isEmpty());
            QCOMPARE(controller.draft(), "Keep one message through a lost acknowledgement");
        } else {
            // Automatic GET reconciliation may have already consumed the exact
            // unchanged draft. Otherwise the explicit send retries its intent.
            if (!controller.draft().isEmpty()) controller.send();
            QTRY_VERIFY(controller.draft().isEmpty());
        }
        bool flushed = false; drafts.flush(this, [&](bool okay) { flushed = okay; }); QTRY_VERIFY(flushed);
        storage.shutdown(); QTRY_VERIFY(storage.stopped());
    }

    void outgoingCommittedWithoutAcknowledgementSurvivesProcessRestart()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid()); Fixture fixture;
        QHash<QString, QJsonObject> saved;
        QVector<QString> clientIds;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                const auto id = request.body.value("uuid").toString();
                clientIds.append(id);
                const auto placement = QUuid::createUuidV5(QUuid(topicId), id.toUtf8()).toString(QUuid::WithoutBraces);
                saved[placement] = message(request.body.value("payload").toObject().value("content").toString(), placement);
                return clientIds.size() == 1 ? Response{QJsonObject{}, 503} : Response{saved[placement]};
            }
            if (!request.target.path().endsWith('/')) {
                const auto found = saved.constFind(request.target.path().section('/', -1));
                return found == saved.cend() ? Response{QJsonObject{}, 404} : Response{*found};
            }
            return Response{QJsonArray{}};
        };
        for (const auto &phase : {QStringLiteral("admit"), QStringLiteral("recover")}) {
            QProcess child; auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("CASSI_OUTGOING_CHILD", phase);
            environment.insert("CASSI_OUTGOING_DIRECTORY", directory.path());
            environment.insert("CASSI_OUTGOING_ORIGIN", fixture.server.origin().toString());
            child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(), {"outgoingRestartProcessChild"});
            QVERIFY(child.waitForStarted());
            QVERIFY(QTest::qWaitFor([&] { return child.state() == QProcess::NotRunning; }, 15000));
            const auto output = child.readAllStandardOutput() + child.readAllStandardError();
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QVERIFY2(child.exitCode() == 0, output.constData());
        }
        QCOMPARE(clientIds.size(), 1); // Reconcile the original, never create a second message.
        QCOMPARE(saved.size(), 1);
    }

    void uncertainSendKeepsUuidAcrossAnotherConversation()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        QVector<QString> firstTopicUuids;
        QHash<QString, QJsonObject> saved;
        fixture.messages = [&firstTopicUuids, &saved](const Request &request) {
            const auto path = request.target.path();
            if (request.method == "POST") {
                const auto topic = request.body.value("topic_uuid").toString();
                const auto id = request.body.value("uuid").toString();
                const auto json = message(request.body.value("payload").toObject().value("content").toString(), id, topic);
                saved.insert(id, json);
                if (topic == topicId) {
                    firstTopicUuids.append(id);
                    if (firstTopicUuids.size() == 1)
                        return Response{QJsonObject{}, 503};
                }
                return Response{json};
            }
            if (!path.endsWith('/'))
                return Response{saved.value(path.section('/', -1))};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Uncertain first send");
        fixture.controller.send();
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(!fixture.controller.error().isEmpty());
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.setDraft("Confirmed second send");
        fixture.controller.send();
        QTRY_VERIFY(fixture.controller.draft().isEmpty());
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.draft(), "Uncertain first send");
        fixture.controller.send();
        QTRY_COMPARE(firstTopicUuids.size(), 2);
        QCOMPARE(firstTopicUuids.at(0), firstTopicUuids.at(1));
        QTRY_VERIFY(fixture.controller.draft().isEmpty());
    }

    void controllerKeepsOnlyOneOrdinaryFallbackUnderDraftPressure()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        const auto topicFor = [](int i) { return QString("a1000000-0000-4000-8000-%1").arg(i, 12, 16, QLatin1Char('0')); };
        fixture.topics = [&](const Request &) {
            QJsonArray rows{creationTopic(topicId, streamId, "Initial")};
            for (int i = 0; i < 80; ++i) rows.append(creationTopic(topicFor(i), streamId, "Draft " + QString::number(i)));
            return Response{rows};
        };
        QVERIFY(fixture.start());
        for (int i = 0; i < 80; ++i) {
            const auto topic = topicFor(i); fixture.controller.selectTopic(topic);
            QTRY_VERIFY(drafts.ready(streamId, topic));
            const auto body = QString::number(i) + QString(39990, QChar(0x754c));
            fixture.controller.setDraft(body);
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
            QCOMPARE(fixture.controller.draft(), body);
        }
        const auto usage = fixture.controller.draftMemory();
        QVERIFY2(usage.textBytes <= 3 * 40000 * qsizetype(sizeof(QChar)), qPrintable(QString::number(usage.textBytes)));
        QVERIFY(usage.entries <= 1);
        QVERIFY(usage.verifiedRoutes <= NativeDrafts::MaximumCachedRoutes + 1);
        fixture.controller.selectTopic(topicFor(0)); QTRY_VERIFY(drafts.ready(streamId, topicFor(0)));
        QCOMPARE(fixture.controller.draft(), QString("0") + QString(39990, QChar(0x754c)));
    }

    void pendingAttachmentKeepsItsCaptionAcrossMetadataEviction()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        const auto topicFor = [](int i) { return QString("a2000000-0000-4000-8000-%1").arg(i, 12, 16, QLatin1Char('0')); };
        fixture.topics = [&](const Request &) {
            QJsonArray rows{creationTopic(topicId, streamId, "Initial")};
            for (int i = 0; i < 140; ++i) rows.append(creationTopic(topicFor(i), streamId, "Draft " + QString::number(i)));
            return Response{rows};
        };
        QFile file(directory.filePath("fixture.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
        std::function<void()> complete; const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "fixture.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Initial caption"); fixture.controller.attachFiles({QUrl::fromLocalFile(file.fileName())});
        QTRY_VERIFY(bool(complete)); fixture.controller.setDraft("Newer caption");
        for (int i = 0; i < 140; ++i) {
            fixture.controller.selectTopic(topicFor(i)); QTRY_VERIFY(drafts.ready(streamId, topicFor(i)));
        }
        complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QCOMPARE(fixture.controller.draft(), QString("Newer caption"));
        QVERIFY(fixture.controller.uploadEntries().isEmpty());
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
    }

    void durableComposerRetainsNewerSameTextWhileSendIsBeingVerified_data()
    {
        QTest::addColumn<bool>("durable");
        QTest::newRow("memory") << false;
        QTest::newRow("disk") << true;
    }

    void durableComposerRetainsNewerSameTextWhileSendIsBeingVerified()
    {
        QFETCH(bool, durable);
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.extraTopic = true;
        if (durable) fixture.controller.useDrafts(&drafts);
        QJsonObject saved; std::function<void()> confirm;
        const auto release = qScopeGuard([&] { if (confirm) confirm(); });
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                saved = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{saved};
            }
            if (request.target.path().endsWith(returnedId)) {
                Response response{saved}; response.defer = [&](auto send) { confirm = std::move(send); }; return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); if (durable) QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("A"); fixture.controller.send(); QTRY_VERIFY(bool(confirm));
        fixture.controller.setDraft("B"); fixture.controller.setDraft("A");
        fixture.controller.selectTopic(secondTopicId); fixture.controller.selectTopic(topicId);
        confirm(); confirm = {};
        QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(fixture.controller.draft(), "A");
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
    }

    void durableSendConsumesOnlyAfterCanonicalReadBack()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        QJsonObject saved; bool allowReadBack = false;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                saved = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{saved};
            }
            if (request.target.path().endsWith(returnedId)) return Response{allowReadBack ? saved : QJsonObject{}, allowReadBack ? 200 : 503};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Confirmed only"); fixture.controller.send();
        QTRY_VERIFY(!fixture.controller.error().isEmpty()); QCOMPARE(fixture.controller.draft(), "Confirmed only");
        allowReadBack = true; fixture.controller.send(); QTRY_VERIFY(fixture.controller.draft().isEmpty());
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        service.shutdown(); QTRY_VERIFY(service.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(drafts.owner()), NativeDraftService::Failure::None);
        QVERIFY(!store.latest(streamId, topicId).record);
    }

    void draftStorageFailurePausesDurableSendingAndKeepsInput()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture;
        QVERIFY(fixture.start());
        const auto identity = QJsonDocument(QJsonArray{fixture.api.server().toString(QUrl::FullyEncoded), projectId, userId})
            .toJson(QJsonDocument::Compact);
        const auto owner = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
        NativeDraftStore lock(directory.path()); QCOMPARE(lock.open(owner), NativeDraftService::Failure::None);
        fixture.controller.useDrafts(&drafts);
        QTRY_VERIFY(drafts.failure() != NativeDraftService::Failure::None);
        QVERIFY(fixture.controller.error().isEmpty()); QVERIFY(!fixture.controller.notice().isEmpty());
        fixture.controller.setDraft("Keep input despite a storage failure");
        QTRY_VERIFY(drafts.failure() != NativeDraftService::Failure::None);
        QCOMPARE(fixture.controller.draft(), "Keep input despite a storage failure");
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.send();
        QVERIFY(std::none_of(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &r) {
            return r.method == "POST" && r.target.path().contains("/messages/");
        }));
        lock.close(); drafts.retry();
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        QTRY_VERIFY(fixture.controller.notice().isEmpty());
        QTRY_VERIFY(fixture.controller.canSend());
    }

    void protectedFragmentsAreNotSentAsOrdinaryPlaintext()
    {
        Fixture fixture; QVERIFY(fixture.start()); fixture.controller.setDraft(":::test-only protected fragment:::");
        QVERIFY(!fixture.controller.canSend()); fixture.controller.send();
        QVERIFY(std::none_of(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &r) {
            return r.method == "POST" && r.target.path().contains("/messages/");
        }));
    }

    void durableDraftsFollowTheVerifiedAccountAcrossLogoutAndLogin()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        const auto anotherUser = secondTopicId;
        bool other = false;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (other && request.target.path().endsWith("/actions/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", QJsonObject{{"uuid", anotherUser}}}};
            return response;
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        const auto firstOwner = drafts.owner();
        fixture.controller.setDraft("First account input");
        fixture.controller.logout(); QVERIFY(fixture.controller.draft().isEmpty()); QVERIFY(drafts.owner().isEmpty());
        other = true; QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QVERIFY(drafts.owner() != firstOwner); QVERIFY(fixture.controller.draft().isEmpty());
        fixture.controller.setDraft("Second account input");
        fixture.controller.logout(); other = false; QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId)); QCOMPARE(drafts.owner(), firstOwner);
        QTRY_COMPARE(fixture.controller.draft(), "First account input");
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
    }

    void sendConfirmationClearsMatchingDraftAfterRouteRoundTrip()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        auto saved = message();
        fixture.messages = [&saved](const Request &request) {
            if (request.method == "POST") {
                saved = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{saved};
            }
            if (request.target.path().endsWith(returnedId))
                return Response{saved, 200, 200};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Return to the original conversation");
        fixture.controller.send();
        QTRY_VERIFY(std::any_of(fixture.server.requests.begin(), fixture.server.requests.end(), [](const Request &request) {
            return request.target.path().endsWith(returnedId);
        }));
        fixture.controller.selectTopic(secondTopicId);
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(fixture.controller.draft().isEmpty());
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QCOMPARE(fixture.controller.messages().first().toMap().value("id").toString(), returnedId);
    }

    void encryptedConversationCannotSendPlaintext()
    {
        Fixture fixture;
        fixture.encrypted = true;
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Must not be sent");
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.send();
        QVERIFY(std::none_of(fixture.server.requests.begin(), fixture.server.requests.end(), [](const Request &request) {
            return request.method == "POST" && request.target.path().contains("/messages/");
        }));
    }
};

QTEST_MAIN(ControllerOutboxTest)
#include "native-controller-outbox-test.moc"
