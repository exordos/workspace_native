#include "support/native-test-harness.h"

namespace
{
    const QString providerUuid = QStringLiteral("77777777-7777-4777-8777-000000000007");
    const QString policyRoute = QStringLiteral("/api/workspace/v1/messenger/external_provider_policies/zulip");
    const QString healthRoute = QStringLiteral("/api/workspace/v1/messenger/external_provider_health/zulip");

    QJsonObject policyDto(int revision = 3, bool enabled = true, bool suspended = false,
        qint64 accounts = 5, qint64 chats = 50, qint64 fileBytes = 10 * 1048576,
        QJsonValue ca = QJsonValue::Null)
    {
        return QJsonObject { { "uuid", providerUuid }, { "provider", "zulip" },
            { "created_at", "2026-01-02T10:00:00Z" },
            { "updated_at", "2026-11-20T10:00:00Z" }, { "revision", revision },
            { "enabled", enabled }, { "emergency_suspended", suspended },
            { "limits", QJsonObject { { "max_accounts", accounts },
                { "max_selected_chats_per_account", chats }, { "max_file_bytes", fileBytes } } },
            { "custom_ca_bundle", ca } };
    }

    QJsonObject healthDto()
    {
        return QJsonObject { { "provider", "zulip" }, { "status", "healthy" },
            { "account_counts", QJsonObject { { "synced", 2 }, { "failed", 0 } } },
            { "chat_counts", QJsonObject { { "selected", 3 } } },
            { "bridge_counts", QJsonObject { { "online", 1 } } },
            { "operation_counts", QJsonObject { { "succeeded", 4 } } },
            { "metrics", QJsonObject { { "queue_depth", 0 }, { "selected_chats", 3 },
                { "synchronized_messages", 42 }, { "synchronized_users", 2 } } },
            { "updated_at", "2026-11-22T10:00:00Z" } };
    }

    QJsonObject caDto()
    {
        return QJsonObject { { "uuid", QStringLiteral("66666666-6666-4666-8666-000000000006") },
            { "generation", 1 },
            { "sha256", QString(63, QLatin1Char('a')) + QLatin1Char('b') },
            { "certificate_count", 2 } };
    }
}

class ControllerExtProviderTest final : public NativeControllerTestHarness {
    Q_OBJECT
    struct Server {
        Fixture fixture;
        QJsonObject policy = policyDto();
        QByteArray etag = "\"3\"";
        QJsonObject health = healthDto();
        bool forbid = false, healthOk = true, failAction = false;
        int policyGets = 0, policyPuts = 0, healthGets = 0, suspendPosts = 0, resumePosts = 0;
        QByteArray lastIfMatch;
        QJsonObject lastPutBody;
        std::function<void(std::function<void()>)> deferSuspend;
        std::function<void(std::function<void()>)> deferPolicyGet;
        // Test override for the policy GET reply; when set it wins over the
        // store path (used to hold a late FORBIDDEN reply, #735 review).
        std::function<Response(const Request&)> getOverride;
        // Test override for the PUT reply; when set it wins over the store path.
        std::function<Response(const Request&)> putOverride;

        Response handle(const Request& request)
        {
            const auto path = request.target.path();
            if (path == policyRoute || path.startsWith(policyRoute + "/actions/")) {
                if (request.method == "GET") {
                    ++policyGets;
                    if (getOverride)
                        return getOverride(request);
                    if (forbid)
                        return Response { QJsonObject { { "type", "ExternalResourceForbiddenError" } }, 403, 0, { } };
                    Response response { policy, 200, 0, { { "ETag", etag } } };
                    response.defer = deferPolicyGet;
                    return response;
                }
                if (request.method == "PUT") {
                    ++policyPuts;
                    lastIfMatch = request.headers.value("if-match");
                    lastPutBody = request.body;
                    if (putOverride)
                        return putOverride(request);
                    const auto settings = request.body.value("settings").toObject();
                    const auto limits = settings.value("limits").toObject();
                    policy = policyDto(policy.value("revision").toInt() + 1,
                        settings.value("enabled").toBool(), policy.value("emergency_suspended").toBool(),
                        static_cast<qint64>(limits.value("max_accounts").toDouble()),
                        static_cast<qint64>(limits.value("max_selected_chats_per_account").toDouble()),
                        static_cast<qint64>(limits.value("max_file_bytes").toDouble()));
                    etag = "\"" + QByteArray::number(policy.value("revision").toInt()) + "\"";
                    return Response { policy, 200, 0, { { "ETag", etag } } };
                }
                if (request.method == "POST" && path.endsWith("/actions/suspend/invoke")) {
                    ++suspendPosts;
                    if (failAction)
                        return Response { QJsonObject { { "type", "InternalError" } }, 500, 0, { } };
                    Response response { QJsonObject(), 200, 0, { { "ETag", etag } } };
                    response.defer = deferSuspend;
                    if (deferSuspend) {
                        // The held reply, when released, carries the suspended DTO.
                        auto held = policy;
                        held["emergency_suspended"] = true;
                        held["revision"] = held.value("revision").toInt() + 1;
                        etag = "\"" + QByteArray::number(held.value("revision").toInt()) + "\"";
                        response.data = held;
                        response.headers.insert("ETag", etag);
                        return response;
                    }
                    policy["emergency_suspended"] = true;
                    policy["revision"] = policy.value("revision").toInt() + 1;
                    response.data = policy;
                    return response;
                }
                if (request.method == "POST" && path.endsWith("/actions/resume/invoke")) {
                    ++resumePosts;
                    policy["emergency_suspended"] = false;
                    policy["revision"] = policy.value("revision").toInt() + 1;
                    etag = "\"" + QByteArray::number(policy.value("revision").toInt()) + "\"";
                    return Response { policy, 200, 0, { { "ETag", etag } } };
                }
                return Response { QJsonObject { }, 405, 0, { } };
            }
            if (path == healthRoute && request.method == "GET") {
                ++healthGets;
                if (!healthOk)
                    return Response { QJsonObject { { "type", "ExternalResourceUnavailableError" } }, 503, 0, { } };
                return Response { health, 200, 0, { } };
            }
            return sessionResponse(request);
        }
        bool start()
        {
            fixture.authentication = [this](const Request& request) { return handle(request); };
            return fixture.start();
        }
        NativeExternalProviderAdmin* admin() { return fixture.controller.externalProviderAdmin(); }
    };

private slots:
    void probeGrantsAccessAndBaselinesDraft()
    {
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        QCOMPARE(admin->accessStatus(), QString("unknown"));
        QVERIFY(!admin->canSave());
        admin->probe();
        QTRY_COMPARE(test.policyGets, 1);
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        QCOMPARE(admin->policyStatus(), QString("ready"));
        QVERIFY(!admin->dirty());
        QVERIFY(!admin->caBlocked());
        QVERIFY(!admin->canSave());
        QCOMPARE(admin->policyEtag(), QByteArray("\"3\""));
        const auto policy = admin->policy();
        QCOMPARE(policy.value("uuid").toString(), providerUuid);
        QCOMPARE(policy.value("enabled").toBool(), true);
        QCOMPARE(test.policyGets, 1); // The probe loads the policy itself; no second GET.
    }

    void probeDeniesForbiddenAndUnauthorized()
    {
        Server forbidden;
        forbidden.forbid = true;
        QVERIFY(forbidden.start());
        forbidden.admin()->probe();
        QTRY_COMPARE(forbidden.admin()->accessStatus(), QString("denied"));
        QCOMPARE(forbidden.admin()->policyStatus(), QString("idle"));
        QVERIFY(!forbidden.admin()->dialogOpen());
        forbidden.admin()->openDialog();
        QVERIFY(!forbidden.admin()->dialogOpen());

        Server unauthenticated;
        QVERIFY(unauthenticated.start());
        unauthenticated.fixture.authentication = [](const Request& request) {
            const auto path = request.target.path();
            if (path == policyRoute)
                return Response { QJsonObject { { "type", "AuthenticationRequiredError" } }, 401, 0, { } };
            return sessionResponse(request);
        };
        unauthenticated.admin()->probe();
        QTRY_COMPARE(unauthenticated.admin()->accessStatus(), QString("denied"));
    }

    void saveSendsIfMatchAndRebaselinesDraft()
    {
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        admin->setLimit("max_accounts", 7);
        QVERIFY(admin->dirty());
        QVERIFY(admin->canSave());
        admin->save();
        QTRY_COMPARE(test.policyPuts, 1);
        QCOMPARE(test.lastIfMatch, QByteArray("\"3\""));
        const auto settings = test.lastPutBody.value("settings").toObject();
        QCOMPARE(settings.value("kind").toString(), QString("zulip"));
        QCOMPARE(settings.value("enabled").toBool(), true);
        QCOMPARE(settings.value("limits").toObject().value("max_accounts").toDouble(), 7.0);
        QVERIFY(settings.contains("custom_ca_bundle"));
        QVERIFY(settings.value("custom_ca_bundle").isNull());
        QTRY_VERIFY(!admin->saveBusy());
        QCOMPARE(admin->policy().value("limits").toMap().value("max_accounts").toLongLong(), 7LL);
        QVERIFY(!admin->dirty());
        QVERIFY(!admin->canSave());
        QCOMPARE(admin->policyEtag(), QByteArray("\"4\""));
    }

    void saveConflictKeepsLocalDraftUntilExplicitChoice()
    {
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        admin->setEnabled(false);
        // Another admin changed the server policy meanwhile; the PUT answers 412
        // with the fresh snapshot and etag.
        auto fresh = policyDto(9, true, false, 9, 50);
        fresh.insert("type", "ExternalPreconditionFailedError");
        test.putOverride = [&fresh](const Request&) {
            return Response { fresh, 412, 0, { { "ETag", "\"9\"" } } };
        };
        admin->save();
        QTRY_VERIFY(admin->conflict());
        QVERIFY(!admin->saveBusy());
        QVERIFY(!admin->saveFailed()); // A conflict is a notice, not a failure.
        // The local edit survives the conflict notice.
        QCOMPARE(test.lastIfMatch, QByteArray("\"3\""));
        QVERIFY(admin->dirty());
        // "Keep my draft": notice clears, edit stays, retry uses the fresh etag.
        admin->keepDraft();
        QVERIFY(!admin->conflict());
        QCOMPARE(admin->draft().value("enabled").toBool(), false);
        // "Load current settings": draft falls back to the fresh policy.
        admin->resetDraft();
        QVERIFY(!admin->dirty());
        QCOMPARE(admin->draft().value("enabled").toBool(), true);
    }

    void policyRefreshRebasesLocalEditsThreeWay()
    {
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        admin->setLimit("max_accounts", 11); // Local edit.
        // Server meanwhile changed only the chat cap (50 -> 77).
        test.policy = policyDto(4, true, false, 5, 77);
        test.etag = "\"4\"";
        admin->refreshPolicy();
        QTRY_COMPARE(test.policyGets, 2);
        QTRY_COMPARE(admin->policy().value("limits").toMap().value("max_selected_chats_per_account").toLongLong(), 77LL);
        // The locally edited field keeps the local value; the untouched field
        // adopts the incoming one (three-way rebase contract).
        QCOMPARE(admin->draft().value("max_accounts").toLongLong(), 11LL);
        QCOMPARE(admin->draft().value("max_selected_chats_per_account").toLongLong(), 77LL);
        QVERIFY(admin->dirty());
        QVERIFY(admin->canSave());
    }

    void conflictKeepDraftRetryRebasesUntouchedFields()
    {
        // Issue #735: after a 412 + "Keep my draft", the retry must keep only
        // the locally edited fields and adopt the server values for every
        // untouched field, so another administrator's change survives.
        Server test;
        QVERIFY(test.start());
        auto admin = test.fixture.controller.externalProviderAdmin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        admin->setLimit("max_accounts", 11); // The only local edit.
        // Another admin saved the chat cap meanwhile (50 -> 77); the server
        // policy now carries 5 accounts and 77 chats against our old etag.
        auto fresh = policyDto(9, true, false, 5, 77);
        fresh.insert("type", "ExternalPreconditionFailedError");
        int putAttempt = 0;
        test.putOverride = [&fresh, &putAttempt](const Request& request) {
            ++putAttempt;
            if (putAttempt == 1)
                return Response { fresh, 412, 0, { { "ETag", "\"9\"" } } };
            const auto settings = request.body.value("settings").toObject();
            const auto limits = settings.value("limits").toObject();
            return Response { policyDto(10, settings.value("enabled").toBool(), false,
                                      static_cast<qint64>(limits.value("max_accounts").toDouble()),
                                      static_cast<qint64>(limits.value("max_selected_chats_per_account").toDouble()),
                                      static_cast<qint64>(limits.value("max_file_bytes").toDouble())),
                200, 0, { { "ETag", "\"10\"" } } };
        };
        admin->save();
        QTRY_VERIFY(admin->conflict());
        // The conflict reply settles the policy: the fresh chat cap is visible
        // and the draft rebases field-wise (local edit kept, untouched field adopts 77).
        QTRY_COMPARE(admin->policy().value("limits").toMap().value("max_selected_chats_per_account").toLongLong(), 77LL);
        QCOMPARE(admin->draft().value("max_accounts").toLongLong(), 11LL);
        QCOMPARE(admin->draft().value("max_selected_chats_per_account").toLongLong(), 77LL);
        QVERIFY(admin->dirty());
        // "Keep my draft" then Save: the full retry body carries 11 accounts
        // AND the other administrator's 77 chats under the fresh etag.
        admin->keepDraft();
        QVERIFY(!admin->conflict());
        QVERIFY(admin->canSave());
        admin->save();
        QTRY_COMPARE(putAttempt, 2);
        QCOMPARE(test.lastIfMatch, QByteArray("\"9\""));
        const auto retried = test.lastPutBody.value("settings").toObject();
        const auto retriedLimits = retried.value("limits").toObject();
        QCOMPARE(retriedLimits.value("max_accounts").toDouble(), 11.0);
        QCOMPARE(retriedLimits.value("max_selected_chats_per_account").toDouble(), 77.0);
        // The confirmed policy holds both administrators' values.
        QTRY_VERIFY(!admin->saveBusy());
        QCOMPARE(admin->policy().value("limits").toMap().value("max_accounts").toLongLong(), 11LL);
        QCOMPARE(admin->policy().value("limits").toMap().value("max_selected_chats_per_account").toLongLong(), 77LL);
        QVERIFY(!admin->dirty());
    }

    void conflictRebaseBaseSurvivesInFlightRefresh()
    {
        // exordy medium on #751: a refresh GET that was dispatched BEFORE the
        // save but replies while the PUT is pending must not swap the rebase
        // base - the user's local edit would match the refreshed value and be
        // dropped as "untouched" at the 412. The reply must be discarded while
        // the mutation is in flight.
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        QCOMPARE(test.policyGets, 1);
        admin->setLimit("max_accounts", 11); // The only local edit (5 -> 11).
        // A peer admin meanwhile set the same value server-side (rev 6 carries
        // accounts 11); the refresh starts and its reply is held.
        test.policy = policyDto(6, true, false, 11, 50);
        test.etag = "\"6\"";
        std::function<void()> releaseGet;
        test.deferPolicyGet = [&releaseGet](std::function<void()> send) { releaseGet = std::move(send); };
        admin->refreshPolicy();
        QTRY_COMPARE(test.policyGets, 2);
        // The save dispatches while the refresh is still in flight; its 412
        // reply (held too) carries a newer peer change (accounts 9, chats 77).
        auto fresh = policyDto(9, true, false, 9, 77);
        fresh.insert("type", "ExternalPreconditionFailedError");
        int putAttempt = 0;
        std::function<void()> releasePut;
        test.putOverride = [&fresh, &putAttempt, &releasePut](const Request&) {
            ++putAttempt;
            if (putAttempt == 1) {
                Response response { fresh, 412, 0, { { "ETag", "\"9\"" } } };
                response.defer = [&releasePut](std::function<void()> send) { releasePut = std::move(send); };
                return response;
            }
            return Response { policyDto(10, true, false, 9, 77), 200, 0, { { "ETag", "\"10\"" } } };
        };
        admin->save();
        QTRY_VERIFY(admin->saveBusy());
        releaseGet();
        QTest::qWait(50); // The late refresh reply arrives mid-save.
        releasePut();
        QTRY_VERIFY(admin->conflict());
        // Rebase base was the pre-refresh policy: the local accounts edit is
        // kept (NOT swallowed by the peer's matching refresh value), the
        // untouched chats field adopts the 412 snapshot's 77.
        QTRY_COMPARE(admin->draft().value("max_accounts").toLongLong(), 11LL);
        QCOMPARE(admin->draft().value("max_selected_chats_per_account").toLongLong(), 77LL);
        QVERIFY(admin->dirty());
        QVERIFY(admin->canSave());
        admin->keepDraft();
        admin->save();
        QTRY_COMPARE(putAttempt, 2);
        QCOMPARE(test.lastIfMatch, QByteArray("\"9\""));
        const auto retried = test.lastPutBody.value("settings").toObject().value("limits").toObject();
        QCOMPARE(retried.value("max_accounts").toDouble(), 11.0);
        QCOMPARE(retried.value("max_selected_chats_per_account").toDouble(), 77.0);
    }

    void failedMutationReissuesRefreshDroppedInFlight()
    {
        // exordy medium on #751 (finding 4670132621): the guard that drops a
        // refresh reply landing during a mutation must not lose the requested
        // refresh when that mutation later FAILS - the failure paths keep the
        // old policy, so the refresh must be reissued afterwards.
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        QCOMPARE(test.policyGets, 1);
        std::function<void()> releaseGet;
        test.deferPolicyGet = [&releaseGet](std::function<void()> send) { releaseGet = std::move(send); };
        admin->refreshPolicy(); // GET #2 held with the rev-3 snapshot.
        QTRY_VERIFY(releaseGet);
        test.deferPolicyGet = nullptr;
        // The server moves on while the reply is held.
        test.policy = policyDto(5, true, false, 9, 50);
        test.etag = "\"5\"";
        admin->setLimit("max_accounts", 8); // Local edit 5 -> 8.
        QVERIFY(admin->dirty());
        std::function<void()> releasePut;
        test.putOverride = [&releasePut](const Request&) {
            Response response { QJsonObject { { "type", "InternalError" } }, 500, 0, { } };
            response.defer = [&releasePut](std::function<void()> send) { releasePut = std::move(send); };
            return response;
        };
        admin->save();
        QTRY_VERIFY(admin->saveBusy());
        releaseGet(); // The rev-3 refresh lands while the PUT is pending: dropped.
        QTest::qWait(50);
        releasePut(); // The mutation fails.
        QTRY_VERIFY(admin->saveFailed());
        // The dropped refresh is reissued after the failure and adopts the
        // newer server snapshot (rev 5, ETag "5").
        QTRY_COMPARE(test.policyGets, 3);
        QTRY_COMPARE(admin->policy().value("revision").toInt(), 5);
        QCOMPARE(admin->policyEtag(), QStringLiteral("\"5\""));
        // The user's edit and the failure notice survive the re-adoption.
        QVERIFY(admin->dirty());
        QCOMPARE(admin->draft().value("max_accounts").toLongLong(), 8LL);
    }

    void malformedConflictReissuedRefreshKeepsUserDraft()
    {
        // exordy medium on #751 (finding 4670483902): a 412 whose snapshot
        // fails to parse moves the policy to error; the deferred refresh
        // reissued from that failure path must keep the previously parsed
        // policy as its rebase base. Without a base the retry lands with no
        // rebase reference and replaces the user's draft wholesale with the
        // server snapshot.
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        QCOMPARE(test.policyGets, 1);
        std::function<void()> releaseGet;
        test.deferPolicyGet = [&releaseGet](std::function<void()> send) { releaseGet = std::move(send); };
        admin->refreshPolicy(); // GET #2 held in flight.
        QTRY_VERIFY(releaseGet);
        test.deferPolicyGet = nullptr;
        // The server moves on while the reply is held (rev 5, accounts 9).
        test.policy = policyDto(5, true, false, 9, 50);
        test.etag = "\"5\"";
        admin->setLimit("max_accounts", 8); // The only local edit (5 -> 8).
        QVERIFY(admin->dirty());
        std::function<void()> releasePut;
        test.putOverride = [&releasePut](const Request&) {
            // Precondition failed but WITHOUT a parseable policy snapshot:
            // the failure keeps the old policy and marks the policy error.
            const QJsonObject malformed { { "type", "ExternalPreconditionFailedError" } };
            Response response { malformed, 412, 0, { { "ETag", "\"9\"" } } };
            response.defer = [&releasePut](std::function<void()> send) { releasePut = std::move(send); };
            return response;
        };
        admin->save();
        QTRY_VERIFY(admin->saveBusy());
        releaseGet(); // The held refresh lands mid-save: dropped (deferred).
        QTest::qWait(50);
        releasePut(); // The malformed 412: policy error, refresh reissued.
        QTRY_COMPARE(test.policyGets, 3);
        QTRY_COMPARE(admin->policy().value("revision").toInt(), 5);
        QCOMPARE(admin->policyStatus(), QString("ready"));
        // The rebase base survived the error hop: the local edit stays and the
        // untouched fields adopt the newer snapshot (rev 5 keeps chats 50).
        QCOMPARE(admin->draft().value("max_accounts").toLongLong(), 8LL);
        QCOMPARE(admin->draft().value("max_selected_chats_per_account").toLongLong(), 50LL);
        QVERIFY(admin->dirty());
        QVERIFY(admin->canSave());
    }

    void forbiddenRefreshDuringPendingMutationStillRevokesAccess()
    {
        // exordy medium on #751 (finding 4670132627): a refresh answering 401
        // or forbidden-403 while a mutation is in flight must not be dropped by
        // the stale-reply guard - if the mutation then also fails, access would
        // be left granted although the server already revoked it.
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        std::function<void()> releaseGet;
        test.getOverride = [&releaseGet](const Request&) {
            Response response { QJsonObject { { "type", "ExternalResourceForbiddenError" } }, 403, 0, { } };
            response.defer = [&releaseGet](std::function<void()> send) { releaseGet = std::move(send); };
            return response;
        };
        admin->refreshPolicy(); // Forbidden reply held in flight.
        QTRY_VERIFY(releaseGet);
        test.getOverride = nullptr;
        admin->setLimit("max_accounts", 8);
        QVERIFY(admin->dirty());
        std::function<void()> releasePut;
        test.putOverride = [&releasePut](const Request&) {
            Response response { QJsonObject { { "type", "InternalError" } }, 500, 0, { } };
            response.defer = [&releasePut](std::function<void()> send) { releasePut = std::move(send); };
            return response;
        };
        admin->save();
        QTRY_VERIFY(admin->saveBusy());
        releaseGet(); // The revocation reply lands while the PUT is pending.
        QTRY_COMPARE(admin->accessStatus(), QString("denied"));
        releasePut(); // The mutation also fails: access must stay revoked.
        QTRY_VERIFY(admin->saveFailed());
        QCOMPARE(admin->accessStatus(), QString("denied"));
        QCOMPARE(admin->policyStatus(), QString("idle"));
    }

    void limitCapsMarkFieldsInvalidAndDisableSave()
    {
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        admin->setLimit("max_accounts", 100001);
        QVERIFY(!admin->fieldsValid());
        QVERIFY(!admin->canSave());
        admin->setLimit("max_selected_chats_per_account", 1000001);
        QVERIFY(!admin->canSave());
        admin->setLimit("max_file_bytes", 5120LL * 1048576LL + 1);
        QVERIFY(!admin->canSave());
        admin->setLimit("max_file_bytes", 5120LL * 1048576LL);
        admin->setLimit("max_selected_chats_per_account", 1000000);
        admin->setLimit("max_accounts", 100000);
        QVERIFY(admin->fieldsValid());
        QVERIFY(admin->canSave());
        admin->setLimit("max_accounts", -1);
        QVERIFY(!admin->fieldsValid());
        QVERIFY(!admin->canSave());
        QCOMPARE(test.policyPuts, 0);
    }

    void caBundlePresentBlocksSave()
    {
        Server test;
        test.policy = policyDto(3, true, false, 5, 50, 10 * 1048576, caDto());
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        QVERIFY(admin->caBlocked());
        admin->setLimit("max_accounts", 4);
        QVERIFY(admin->dirty());
        QVERIFY(admin->fieldsValid());
        QVERIFY(!admin->canSave()); // CA present => Save stays disabled.
        const auto ca = admin->policy().value("ca").toMap();
        QCOMPARE(ca.value("certificateCount").toInt(), 2);
        QCOMPARE(ca.value("generation").toInt(), 1);
    }

    // Issue #740: a policy GET that started before a confirmed save must not
    // roll back the confirmed revision, ETag or values when it lands late;
    // the form stays clean and a refresh started after the mutation still
    // adopts the newer server snapshot.
    void stalePolicyRefreshCannotRollBackConfirmedSave()
    {
        Server test;
        std::function<void()> release;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        // Only the refresh GET is held - the initial load already landed.
        test.deferPolicyGet = [&release](std::function<void()> send) { release = std::move(send); };
        admin->refreshPolicy(); // Holds the rev-3 snapshot across the save.
        QTRY_VERIFY(release);
        test.deferPolicyGet = nullptr; // Later refreshes flow freely.
        admin->setLimit("max_accounts", 7);
        QVERIFY(admin->dirty());
        admin->save();
        QTRY_COMPARE(test.policyPuts, 1);
        QTRY_VERIFY(!admin->dirty()); // Confirmed draft at revision 4.
        QCOMPARE(admin->policy().value("revision").toInt(), 4);
        release(); // The late GET lands with the rev-3 snapshot and ETag "3".
        QTest::qWait(150);
        // The late response is discarded: revision, values, clean draft survive.
        QCOMPARE(admin->policy().value("revision").toInt(), 4);
        QCOMPARE(admin->policy().value("limits").toMap().value("max_accounts").toLongLong(), 7LL);
        QVERIFY(!admin->dirty());
        QVERIFY(!admin->conflict());
        // A refresh started after the mutation must ADOPT a newer server
        // snapshot, not merely reach the server: advance the server past the
        // confirmed revision first and wait for the client to adopt the rev-5
        // values and ETag (#740 review finding).
        test.policy = policyDto(5, true, false, 9, 50, 10 * 1048576);
        test.etag = "\"5\"";
        admin->refreshPolicy();
        QTRY_COMPARE(test.policyGets, 3);
        QTRY_COMPARE(admin->policy().value("revision").toInt(), 5);
        QCOMPARE(admin->policy().value("limits").toMap().value("max_accounts").toLongLong(), 9LL);
        QCOMPARE(admin->policyEtag(), QStringLiteral("\"5\""));
        QVERIFY(!admin->dirty()); // Adoption keeps the draft clean.
    }

    void suspendRunsPendingThenFlipsAndResumeReturns()
    {
        Server test;
        std::function<void()> release;
        test.deferSuspend = [&release](std::function<void()> send) { release = std::move(send); };
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        admin->suspend();
        QTRY_COMPARE(test.suspendPosts, 1);
        QCOMPARE(admin->actionStatus(), QString("suspending"));
        QVERIFY(!admin->canSave()); // The mutation lock blocks Save while an action runs.
        QTRY_VERIFY(release);
        release();
        QTRY_COMPARE(admin->actionStatus(), QString("none"));
        QTRY_VERIFY(admin->policy().value("emergencySuspended").toBool());
        QVERIFY(!admin->actionFailed());
        admin->resume();
        QTRY_COMPARE(test.resumePosts, 1);
        QTRY_VERIFY(!admin->policy().value("emergencySuspended").toBool());
        // Guard symmetry: no action request when the state already matches.
        const auto posts = test.suspendPosts + test.resumePosts;
        admin->resume();
        QCOMPARE(test.suspendPosts + test.resumePosts, posts);
    }

    void actionFailureNoticesAndKeepsForm()
    {
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        test.failAction = true;
        admin->setEnabled(false);
        const auto draftBefore = admin->draft();
        admin->suspend();
        QTRY_COMPARE(test.suspendPosts, 1);
        QTRY_VERIFY(admin->actionFailed());
        QCOMPARE(admin->actionStatus(), QString("none"));
        QVERIFY(!admin->policy().value("emergencySuspended").toBool());
        QCOMPARE(admin->draft(), draftBefore); // "The form settings were not changed."
        QVERIFY(admin->dirty());
        QVERIFY(admin->canSave()); // Save remains available after a failed action.
        test.failAction = false;
        admin->suspend();
        QTRY_COMPARE(test.suspendPosts, 2);
        QTRY_VERIFY(admin->policy().value("emergencySuspended").toBool());
        QVERIFY(!admin->actionFailed());
    }

    void healthLoadsRetriesAndReportsRefreshError()
    {
        Server test;
        QVERIFY(test.start());
        auto admin = test.admin();
        admin->probe();
        QTRY_COMPARE(admin->accessStatus(), QString("granted"));
        admin->openDialog();
        QVERIFY(admin->dialogOpen());
        QCOMPARE(admin->healthStatus(), QString("loading"));
        QTRY_COMPARE(test.healthGets, 1);
        QTRY_COMPARE(admin->healthStatus(), QString("ready"));
        QCOMPARE(admin->health().value("status").toString(), QString("healthy"));
        QCOMPARE(admin->health().value("accountCounts").toMap().value("synced").toInt(), 2);
        // A failed refresh keeps the last good data and marks refreshError.
        test.healthOk = false;
        admin->refreshHealth();
        QTRY_COMPARE(test.healthGets, 2);
        QTRY_COMPARE(admin->healthStatus(), QString("refreshError"));
        QCOMPARE(admin->health().value("status").toString(), QString("healthy"));
        // Retry self-heals.
        test.healthOk = true;
        admin->refreshHealth();
        QTRY_COMPARE(test.healthGets, 3);
        QTRY_COMPARE(admin->healthStatus(), QString("ready"));
        // Access loss closes the dialog itself.
        test.forbid = true;
        admin->refreshPolicy();
        QTRY_COMPARE(admin->accessStatus(), QString("denied"));
        QVERIFY(!admin->dialogOpen());
    }
};

QTEST_MAIN(ControllerExtProviderTest)
#include "native-controller-ext-provider-test.moc"
