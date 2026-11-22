#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-chat-creation-fixtures.h"

namespace {
// Issue #632: an encrypted stream whose DTO completes the crypto tuple and
// (optionally) confirms allow_key_sharing_by_holders at the given updated_at.
inline QJsonObject keySharingStream(bool owner = true, const QJsonValue &allow = true,
    const QString &updatedAt = QStringLiteral("2026-01-02T10:00:00.000Z"))
{
    auto row = creationChannel(streamId, "Key sharing fixture");
    row["encryption"] = true;
    row["current_encryption_key_uuid"] = QStringLiteral("11111111-1111-4111-8111-111111111111");
    if (!owner) row["owner"] = channelMemberA;
    if (allow.isNull()) row.remove("allow_key_sharing_by_holders");
    else row["allow_key_sharing_by_holders"] = allow;
    row["updated_at"] = updatedAt;
    return row;
}

inline QString streamRowName(const NativeController &controller)
{
    for (const auto &value : controller.streams()) {
        const auto row = value.toMap();
        if (row.value("id").toString() == streamId) return row.value("name").toString();
    }
    return {};
}

class KeySharingToggleFixture {
public:
    Fixture fixture;
    QJsonObject row{keySharingStream()};
    int puts = 0;
    int putStatus = 200, putDelay = 0;
    bool commitDespiteFailure = false;
    QJsonObject putBody;
    QString putEchoUpdated = QStringLiteral("2026-01-02T10:00:05.000100Z");
    std::function<void(QJsonObject &)> alterPut;
    // Test-owned gate for the ambiguous-failure reconcile GET: when set, the
    // stream detail GET is held until the test releases it, so a newer value
    // can deterministically land while that answer is still in flight.
    bool deferReconcile = false;
    std::function<void()> releaseReconcile;
    bool start()
    {
        fixture.streams = [this](const Request &) { return Response{QJsonArray{row}}; };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "PUT" && path == "/api/workspace/v1/messenger/streams/" + streamId) {
                ++puts; putBody = request.body;
                if (commitDespiteFailure)
                    row["allow_key_sharing_by_holders"] = putBody.value("allow_key_sharing_by_holders");
                auto echo = row;
                echo["allow_key_sharing_by_holders"] = putBody.value("allow_key_sharing_by_holders");
                echo["updated_at"] = putEchoUpdated;
                if (alterPut) alterPut(echo);
                return {echo, putStatus, putDelay, {}};
            }
            if (request.method == "GET" && path == "/api/workspace/v1/messenger/streams/" + streamId) {
                if (deferReconcile) {
                    Response held{row};
                    held.defer = [this](std::function<void()> send) {
                        releaseReconcile = std::move(send);
                    };
                    return held;
                }
                return {row, 200, 0, {}};
            }
            return fallback(request);
        };
        return fixture.start();
    }
    // Realtime stream snapshot envelope, the shape the reference applier and
    // the e2ee fixtures use (payload carries the full stream DTO + kind).
    QJsonObject snapshot(const QJsonObject &dto, const QString &kind = "stream.updated")
    {
        auto payload = dto; payload["kind"] = kind;
        return {{"uuid", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"project_id", projectId}, {"user_uuid", userId}, {"payload", payload}};
    }
    QVariantMap projection() const { return fixture.controller.chatKeySharing(); }
};
} // namespace

class ControllerEncryptedChannelsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
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

    // ---- Issue #632: encrypted channel key-sharing toggle ----
    // (docs/NATIVE_DESKTOP_KEY_SHARING_TOGGLE.md)

    void keySharingProjectionIsOwnerOnlyAndRequiresEncryptedDto()
    {
        KeySharingToggleFixture owner; QVERIFY(owner.start());
        // DTO parse: allow_key_sharing_by_holders=true confirms checked=true.
        auto map = owner.projection();
        QCOMPARE(map.value("checked").toBool(), true);
        QCOMPARE(map.value("disabled").toBool(), false);
        owner.fixture.controller.setChatKeySharing(false);
        QTRY_VERIFY(!owner.projection().value("disabled").toBool());
        QCOMPARE(owner.projection().value("checked").toBool(), false);
        // Owner-only editability (editableStreamKeySharing): another owner's
        // encrypted stream never exposes the map.
        KeySharingToggleFixture other; other.row = keySharingStream(false); QVERIFY(other.start());
        QVERIFY(other.projection().isEmpty());
        other.fixture.controller.setChatKeySharing(true);
        QCOMPARE(other.puts, 0); // the mutation is refused without a request
        // A DTO without the flag keeps the setting unknown: hidden, no toggle.
        KeySharingToggleFixture missing; missing.row = keySharingStream(true, QJsonValue::Null); QVERIFY(missing.start());
        QVERIFY(missing.projection().isEmpty());
        // A plain (unencrypted) stream hides the row even for its owner.
        KeySharingToggleFixture plain; plain.row = keySharingStream(true, true);
        plain.row["encryption"] = false; QVERIFY(plain.start());
        QVERIFY(plain.projection().isEmpty());
    }

    void keySharingToggleCommitsOnVerifiedReadback()
    {
        KeySharingToggleFixture test; QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        QCOMPARE(test.projection().value("checked").toBool(), true);
        test.putDelay = 150;
        controller.setChatKeySharing(false);
        QTRY_VERIFY(test.puts == 1);
        // Pending scope: the switch is disabled while the request is in flight
        // (encrypted-chat-info-connected.ui.tsx:254).
        QTRY_VERIFY(test.projection().value("disabled").toBool());
        QCOMPARE(test.putBody, (QJsonObject{{"allow_key_sharing_by_holders", false}}));
        QTRY_VERIFY(!test.projection().value("disabled").toBool());
        QCOMPARE(test.projection().value("checked").toBool(), false);
        QVERIFY(controller.chatKeySharingError().isEmpty());
        // Toggling back flips the committed value and sends the new flag.
        controller.setChatKeySharing(true);
        QTRY_COMPARE(test.puts, 2);
        QCOMPARE(test.putBody, (QJsonObject{{"allow_key_sharing_by_holders", true}}));
        QTRY_VERIFY(test.projection().value("checked").toBool());
        QVERIFY(controller.chatKeySharingError().isEmpty());
        // One in-flight request per stream: a second click while pending is
        // refused until the first settles.
        test.putDelay = 150;
        controller.setChatKeySharing(false);
        QTRY_VERIFY(test.projection().value("disabled").toBool());
        controller.setChatKeySharing(true);
        QTRY_COMPARE(test.puts, 3);
        QTest::qWait(250); // the refused click never becomes a request
        QCOMPARE(test.puts, 3);
        QTRY_VERIFY(!test.projection().value("disabled").toBool());
    }

    void keySharingToggleFailureKeepsPreviousValueAndNotices()
    {
        KeySharingToggleFixture test; QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        test.putStatus = 503;
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        QTRY_VERIFY(!test.projection().value("disabled").toBool());
        // Reference toast (channel.keySharingUpdateFailed): the previous value
        // survives the failed write and the notice names the failure.
        QCOMPARE(test.projection().value("checked").toBool(), true);
        QCOMPARE(controller.chatKeySharingError(), QStringLiteral("Could not update key sharing."));
        // A later successful toggle clears the notice and commits.
        test.putStatus = 200;
        controller.setChatKeySharing(false);
        QTRY_VERIFY(test.projection().value("checked").toBool() == false);
        QVERIFY(controller.chatKeySharingError().isEmpty());
    }

    void keySharingToggleUnverifiedReadbackIsAFailure()
    {
        KeySharingToggleFixture test; QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        // The echo confirms the wrong flag (messenger-sidebar-actions
        // .lib.ts:614-621 readback verification): treat as failure, no commit.
        test.alterPut = [](QJsonObject &echo) { echo["allow_key_sharing_by_holders"] = true; };
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        QTRY_VERIFY(!test.projection().value("disabled").toBool());
        QCOMPARE(test.projection().value("checked").toBool(), true);
        QCOMPARE(controller.chatKeySharingError(), QStringLiteral("Could not update key sharing."));
        // An echo describing another user's stream is refused the same way.
        test.alterPut = [](QJsonObject &echo) { echo["user_uuid"] = channelMemberA; };
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 2);
        QTRY_VERIFY(controller.chatKeySharingError() == QStringLiteral("Could not update key sharing."));
        QCOMPARE(test.projection().value("checked").toBool(), true);
    }

    void keySharingRealtimeSnapshotsRespectTheConfirmedValue()
    {
        KeySharingToggleFixture test; QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        // Confirm false at updated_at 10:00:05.000100Z (the PUT echo).
        controller.setChatKeySharing(false);
        QTRY_VERIFY(test.projection().value("checked").toBool() == false);
        // Older snapshot (same millisecond, smaller fraction): the confirmed
        // value wins - the reference sub-millisecond tiebreak.
        auto stale = keySharingStream(true, true, QStringLiteral("2026-01-02T10:00:05.000050Z"));
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(stale)));
        QCOMPARE(test.projection().value("checked").toBool(), false);
        // A snapshot without the flag at a newer stamp also preserves it.
        auto noFlag = keySharingStream(true, QJsonValue::Null, QStringLiteral("2026-01-02T10:00:06.000Z"));
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(noFlag)));
        QCOMPARE(test.projection().value("checked").toBool(), false);
        // A genuinely newer fraction in the same millisecond may overwrite.
        auto tie = keySharingStream(true, true, QStringLiteral("2026-01-02T10:00:05.000200Z"));
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(tie)));
        QCOMPARE(test.projection().value("checked").toBool(), true);
        // A newer snapshot with the flag overwrites the confirmed value.
        auto newer = keySharingStream(true, false, QStringLiteral("2026-01-02T10:00:07.000Z"));
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(newer)));
        QCOMPARE(test.projection().value("checked").toBool(), false);
        // And the new confirmed stamp now protects the newer value too.
        auto staleAgain = keySharingStream(true, true, QStringLiteral("2026-01-02T10:00:06.500Z"));
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(staleAgain)));
        QCOMPARE(test.projection().value("checked").toBool(), false);
    }

    void keySharingStalePutEchoKeepsNewerConfirmedValue()
    {
        KeySharingToggleFixture test;
        // The echo is stamped BEFORE the realtime snapshot that will land while
        // the PUT is in flight (echo delayed, reference :622-624 stale-echo path).
        test.putDelay = 150;
        test.putEchoUpdated = QStringLiteral("2026-01-02T10:00:01Z");
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        // A genuinely newer realtime snapshot arrives while the PUT is pending.
        auto newer = keySharingStream(true, true, QStringLiteral("2026-01-02T10:00:03Z"));
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(newer)));
        // The old echo lands now, confirming the requested flag at its older
        // stamp: the reference discards it as status "applied" - the newer
        // value survives and no failure toast is shown.
        QTRY_VERIFY(!test.projection().value("disabled").toBool());
        QCOMPARE(test.projection().value("checked").toBool(), true);
        QVERIFY(controller.chatKeySharingError().isEmpty());
    }

    void keySharingEchoAppliesOnlyKeySharingFieldsToTheRow()
    {
        KeySharingToggleFixture test;
        // The echo confirms at a stamp NEWER than the row, so the stale-echo
        // guard passes - but its unrelated fields are the request-time
        // snapshot (messenger-sidebar-actions.lib.ts:626-630).
        test.putDelay = 150;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        // A rename lands while the PUT is in flight: with no updated_at the
        // key-sharing stamp carries over, so only the name is newer than the
        // echo will ever be.
        auto renamed = keySharingStream(true, true);
        renamed.remove("updated_at");
        renamed["name"] = QStringLiteral("Renamed by realtime");
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(renamed)));
        QCOMPARE(streamRowName(controller), QStringLiteral("Renamed by realtime"));
        // The verified echo merges only the key-sharing changes into the
        // latest row; the newer unrelated field survives the confirmation.
        QTRY_VERIFY(!test.projection().value("disabled").toBool());
        QCOMPARE(test.projection().value("checked").toBool(), false);
        QVERIFY(controller.chatKeySharingError().isEmpty());
        QCOMPARE(streamRowName(controller), QStringLiteral("Renamed by realtime"));
    }

    void keySharingRowIgnoresIncompleteCryptoMetadata()
    {
        KeySharingToggleFixture test;
        // The crypto tuple is optional metadata: dropping provider fields hides
        // the security witness, but the reference gate is encryption + dto.owner
        // + the flag alone (messenger-adapters.lib.ts:37, key-sharing :3-10).
        test.row = keySharingStream();
        test.row.remove("provider_uuid");
        test.row.remove("provider_identity");
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        QVERIFY(!test.projection().isEmpty());
        QCOMPARE(test.projection().value("checked").toBool(), true);
        QCOMPARE(test.projection().value("disabled").toBool(), false);
        // The row stays editable: the toggle commits through the same readback.
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        QTRY_VERIFY(test.projection().value("checked").toBool() == false);
        QVERIFY(controller.chatKeySharingError().isEmpty());
    }

    void keySharingAmbiguousFailureReconcilesWithServer()
    {
        KeySharingToggleFixture test;
        // The server commits the write but its 5xx response never proves it:
        // the failed write must reconcile the row from the authoritative catalog.
        test.commitDespiteFailure = true;
        test.putStatus = 503;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        QCOMPARE(controller.chatKeySharingError(), QStringLiteral("Could not update key sharing."));
        QTRY_VERIFY(test.projection().value("checked").toBool() == false);
    }

    // exo-review findings 1+2 on PR #655: the ambiguous-failure reconcile GET
    // must not roll back a newer value that landed while that GET was in
    // flight (reference timestamp precedence, :622-624 stale-echo contract).
    void keySharingStaleReconcileGetKeepsNewerValue()
    {
        KeySharingToggleFixture test;
        test.putStatus = 503; // ambiguous write; the server committed nothing
        test.deferReconcile = true; // hold the reconcile answer in flight
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        QTRY_VERIFY(static_cast<bool>(test.releaseReconcile));
        // A genuinely newer realtime value lands while the GET is in flight.
        auto newer = keySharingStream(true, false, QStringLiteral("2026-01-02T10:00:07Z"));
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(newer)));
        QCOMPARE(test.projection().value("checked").toBool(), false);
        // The stale reconcile answer (allow=true at the older stamp) arrives
        // now: it must be dropped, not resurrect the old flag.
        test.releaseReconcile();
        QTest::qWait(300);
        QCOMPARE(test.projection().value("checked").toBool(), false);
    }

    // Issue #681: the reconcile GET is a snapshot for ONE setting. A realtime
    // rename with no newer key-sharing timestamp must survive its arrival:
    // merging only the key-sharing fields (the confirmed-PUT contract) keeps
    // the newer name, while a wholesale row replacement reverts it.
    void keySharingReconcileKeepsNewerRealtimeName()
    {
        KeySharingToggleFixture test;
        test.putStatus = 503;              // ambiguous write: reply lost
        test.commitDespiteFailure = true;  // the server did commit the toggle
        test.deferReconcile = true;        // hold the reconcile answer in flight
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setChatKeySharing(false);
        QTRY_COMPARE(test.puts, 1);
        QTRY_VERIFY(static_cast<bool>(test.releaseReconcile));
        // A rename lands while the GET is held. It carries the CURRENT flag at
        // an equal updated_at, so it is not a newer key-sharing write: the
        // timestamp-precedence guard must not be what protects this field.
        auto renamed = keySharingStream(true, false, QStringLiteral("2026-01-02T10:00:00.000Z"));
        renamed["name"] = QStringLiteral("Renamed after read");
        QVERIFY(controller.applyRealtimeEvent(test.snapshot(renamed)));
        QCOMPARE(streamRowName(controller), QStringLiteral("Renamed after read"));
        // The held snapshot arrives: the committed flag recovers, and the
        // newer unrelated field (the name) is preserved.
        test.releaseReconcile();
        QTRY_VERIFY(test.projection().value("checked").toBool() == false);
        QCOMPARE(streamRowName(controller), QStringLiteral("Renamed after read"));
    }
};

QTEST_MAIN(ControllerEncryptedChannelsTest)
#include "native-controller-encrypted-channels-test.moc"
