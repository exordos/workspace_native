#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-notifications-fixtures.h"

class ControllerSessionsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void staleOtpRejectionDoesNotClearNewAttempt()
    {
        QVector<ApiClient::Callback> replies;
        Session::RequestExecutor executor = [&](const QByteArray &, const QString &, const QJsonValue &,
                                               const QUrlQuery &, ApiClient::Callback callback, bool,
                                               const QMap<QByteArray, QByteArray> &) {
            replies.append(std::move(callback));
        };
        ApiClient api;
        Session session(&api, nullptr, executor);
        NativeController controller(&api, &session);
        QSignalSpy rejected(&controller, &NativeController::otpCodeRejected);
        ApiResult unauthorized; unauthorized.status = 401;
        controller.login("https://first.example", "first", "password", "");
        QCOMPARE(replies.size(), 1);
        auto initial = replies.takeFirst(); initial(unauthorized);
        QCOMPARE(controller.loginStep(), QString("otp"));
        controller.submitOtp("111111");
        QCOMPARE(replies.size(), 1);
        auto oldReply = replies.takeFirst();
        bool replaced = false;
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (!replaced && controller.error() == "The authentication code is invalid. Try again.") {
                replaced = true;
                controller.backToCredentials();
                controller.login("https://second.example", "second", "password", "");
                auto second = replies.takeFirst(); second(unauthorized);
                controller.submitOtp("222222");
                auto newReply = replies.takeFirst(); newReply(unauthorized);
            }
        });
        oldReply(unauthorized);
        QVERIFY(replaced);
        QCOMPARE(rejected.size(), 1); // only the new rejection belongs to the current form
        QCOMPARE(controller.loginUsername(), QString("second"));
    }

    void loginWizardStartsOnOrganizationStep()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationSettings().isEmpty());
        QVERIFY(!controller.organizationLoading());
        QVERIFY(controller.defaultOrganizationUrl().isEmpty());
        QVERIFY(!controller.isOrganizationUrlValid(QStringLiteral("not a url")));
        QVERIFY(!controller.isOrganizationUrlValid(QStringLiteral("https://bad..host")));
        // Reference new URL(...).origin semantics: userinfo and query are
        // stripped by origin normalization, so the address stays valid.
        QVERIFY(controller.isOrganizationUrlValid(QStringLiteral("https://user@acme.example")));
        QVERIFY(controller.isOrganizationUrlValid(QStringLiteral("https://acme.example/with?query")));
        QVERIFY(controller.isOrganizationUrlValid(QStringLiteral("https://acme.example")));
        QVERIFY(controller.isOrganizationUrlValid(fixture.server.origin().toString()));
        controller.continueFromOrganization(QStringLiteral("not a url"));
        QCOMPARE(controller.error(), QStringLiteral("Server address must be a valid HTTPS URL"));
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(fixture.server.requests.isEmpty());
        QVERIFY(!controller.organizationLoading());
    }

    void loginWizardDiscoveryGatesCredentials()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.organizationSettings().value("realmName").toString(), QStringLiteral("Acme Realm"));
        QCOMPARE(controller.organizationSettings().value("realmIconUrl").toString(), origin + "/avatars/realm.png");
        QCOMPARE(controller.organizationSettings().value("registrationUrl").toString(),
                 QStringLiteral("https://reg.example/signup"));
        QCOMPARE(controller.organizationSettings().value("organizationOrigin").toString(), origin);
        // Blur discovery never advances the wizard by itself.
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        QVERIFY(controller.error().isEmpty());
        // The checked origin advances without a second discovery request.
        const auto discoveryCount = std::count_if(fixture.server.requests.cbegin(),
            fixture.server.requests.cend(), [](const Request &request) {
                return request.target.path().endsWith("/server_settings");
            });
        QCOMPARE(discoveryCount, 1);
    }

    void loginWizardPendingAdvanceCompletesWithResponse()
    {
        Fixture fixture;
        std::function<void()> release;
        fixture.serverSettings = [&release](const Request &) {
            Response response{organizationSettingsDto()};
            response.defer = [&release](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationLoading());
        QTRY_VERIFY(static_cast<bool>(release)); // the discovery request must reach the mock first
        QTRY_VERIFY(static_cast<bool>(release));
        release();
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        QVERIFY(controller.error().isEmpty());
    }

    void loginWizardFailureKeepsCredentialsHidden()
    {
        Fixture fixture;
        std::function<void()> release;
        fixture.serverSettings = [&release](const Request &) {
            Response response{QJsonObject{}, 503};
            response.defer = [&release](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.continueFromOrganization(origin);
        QTRY_VERIFY(static_cast<bool>(release));
        release();
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationSettings().isEmpty());
        QCOMPARE(controller.error(),
                 QStringLiteral("Could not load organization settings. Check the server address and try again."));
        const auto discoveryCount = std::count_if(fixture.server.requests.cbegin(),
            fixture.server.requests.cend(), [](const Request &request) {
                return request.target.path().endsWith("/server_settings");
            });
        // A checked failed origin repeats the copy instead of refetching.
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QCOMPARE(controller.error(),
                 QStringLiteral("Could not load organization settings. Check the server address and try again."));
        const auto repeatedCount = std::count_if(fixture.server.requests.cbegin(),
            fixture.server.requests.cend(), [](const Request &request) {
                return request.target.path().endsWith("/server_settings");
            });
        QCOMPARE(repeatedCount, discoveryCount);
    }

    void loginWizardAdvanceNormalizesRealmToOrigin()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        // Discovery and sign-in must both act on the normalized origin, the
        // way the reference submits `new URL(realm).origin` after the check.
        const auto noisy = origin + "/join/team?next=1";
        controller.organizationInputChanged(noisy);
        controller.continueFromOrganization(noisy);
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        QCOMPARE(controller.organizationRealm(), origin);
        controller.login(controller.organizationRealm(), QStringLiteral("synthetic-user"),
            QStringLiteral("synthetic-password"), QString{});
        QTRY_COMPARE(controller.projects().size(), 1);
    }

    void loginWizardStaleDiscoveryIsIgnored()
    {
        Fixture fixture;
        std::vector<std::function<void()>> releases;
        int call = 0;
        fixture.serverSettings = [&releases, &call](const Request &) {
            Response response{organizationSettingsDto(call == 0
                ? QStringLiteral("First Realm") : QStringLiteral("Second Realm"))};
            response.defer = [&releases, &call](std::function<void()> send) {
                releases.push_back(std::move(send));
                ++call;
            };
            return response;
        };
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        // A newer check supersedes the in-flight one (reference fetchIdRef guard).
        controller.organizationInputChanged(QStringLiteral("https://other.example"));
        controller.discoverOrganization(origin);
        QTRY_VERIFY(call == 2);
        releases[1]();
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.organizationSettings().value("realmName").toString(), QStringLiteral("Second Realm"));
        releases[0]();
        QTest::qWait(50);
        QCOMPARE(controller.organizationSettings().value("realmName").toString(), QStringLiteral("Second Realm"));
        QVERIFY(!controller.organizationLoading());
    }

    void loginWizardRealmEditInvalidatesCheck()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        QTRY_VERIFY(!controller.organizationLoading());
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        // Editing the address to a different origin returns to the
        // organization step and discards the completed check.
        MockServer second;
        second.handler = [](const Request &request) {
            if (request.target.path().endsWith("/server_settings"))
                return Response{organizationSettingsDto()};
            return sessionResponse(request);
        };
        const auto otherOrigin = second.origin().toString();
        controller.organizationInputChanged(otherOrigin);
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationSettings().isEmpty());
        controller.discoverOrganization(otherOrigin);
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.organizationSettings().value("organizationOrigin").toString(), otherOrigin);
        QCOMPARE(controller.organizationSettings().value("realmIconUrl").toString(),
                 otherOrigin + "/avatars/realm.png");
    }

    void defaultOrganizationShortcutFillsRealmAndClearsState()
    {
        qputenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL", "https://public.example");
        qputenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME", "Public Test Org");
        Fixture fixture;
        auto &controller = fixture.controller;
        QCOMPARE(controller.defaultOrganizationUrl(), QStringLiteral("https://public.example"));
        QCOMPARE(controller.defaultOrganizationName(), QStringLiteral("Public Test Org"));
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        QTRY_VERIFY(!controller.organizationLoading());
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        controller.selectDefaultOrganization();
        QCOMPARE(controller.organizationRealm(), QStringLiteral("https://public.example"));
        QVERIFY(controller.organizationSettings().isEmpty());
        QVERIFY(controller.error().isEmpty());
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL");
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME");
    }

    void defaultOrganizationNameFallsBackToTranslatedCopy()
    {
        qputenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL", "https://public.example");
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME");
        Fixture fixture;
        QCOMPARE(fixture.controller.defaultOrganizationName(), QStringLiteral("Exordos Core Public"));
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL");
    }

    void loginRecordsOrganizationHistory()
    {
        QTemporaryDir historyDirectory;
        QVERIFY(historyDirectory.isValid());
        const auto storePath = historyDirectory.filePath("history.ini");
        NativePreferences preferences(storePath);
        Fixture fixture;
        auto &controller = fixture.controller;
        controller.setPreferences(&preferences);
        const auto origin = fixture.server.origin().toString();
        controller.login(origin, QStringLiteral("synthetic-user"), QStringLiteral("synthetic-password"), QString{});
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(preferences.organizationHistory().size(), 1);
        QCOMPARE(preferences.organizationHistory().first(), QUrl(origin).toString());
        // Path fragments normalize to the origin and dedupe to the freshest slot.
        preferences.recordOrganizationOrigin(QUrl(origin).toString() + "/some/path");
        QCOMPARE(preferences.organizationHistory().size(), 1);
        NativePreferences reopened(storePath);
        QCOMPARE(reopened.organizationHistory(), preferences.organizationHistory());
    }

    // The controller-side half of docs/NATIVE_DESKTOP_AUTO_PAGING.md: a fetched
    // page whose every row is an account-irrelevant service entry must still
    // commit and move the older marker. The view-side zero-visible auto-advance
    // walks through exactly such pages, so marker traversal has to survive them.
    void allFilteredServicePageStillAdvancesMarkers()
    {
        Fixture fixture;
        const auto announcedRow = [&](int number) {
            auto entry = message(QString("Announced %1").arg(number),
                QString("88000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0')));
            entry["payload"] = QJsonObject{{"kind", "key_announced"},
                {"key_uuid", QString("77000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'))}};
            return entry;
        };
        fixture.messages = [&](const Request &request) -> Response {
            const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
            if (marker.isEmpty()) {
                Response response{QJsonArray{announcedRow(12), announcedRow(11)}};
                response.headers.insert("X-Pagination-Marker", "svc-2");
                return response;
            }
            if (marker == "svc-2")
                return Response{QJsonArray{announcedRow(10), announcedRow(9)}};
            return Response{QJsonObject{}, 404};
        };
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVERIFY(fixture.controller.canLoadOlder()); // a fully filtered page keeps the traversal alive
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 4);
        QVERIFY(!fixture.controller.canLoadOlder()); // the final page carried no marker
    }

    void overlayBannerTransportDropFlipsPhaseOfflineThenRecovers()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.connectionPhase(), QString("online"));
        QCOMPARE(fixture.controller.connectionSeverity(), QString("warning"));
        QVERIFY(!fixture.controller.bootstrapError());
        const auto base = fixture.server.handler;
        // The outage window drops every request the snapshot issues, so an
        // unrelated in-flight success cannot clear the offline phase early.
        fixture.server.handler = [base](const Request &request) {
            const auto path = request.target.path();
            if (request.method == "GET"
                && (path.endsWith("/streams/") || path.endsWith("/users/"))) {
                Response dropped; dropped.abortConnection = true; return dropped;
            }
            return base(request);
        };
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("offline"));
        QCOMPARE(fixture.controller.connectionSeverity(), QString("critical"));
        // Restoring the server and letting a response land clears the banner.
        fixture.server.handler = base;
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("online"));
        QTRY_COMPARE(fixture.controller.streams().size(), 1);
    }

    void overlayBannerRateLimitShowsCountdownUntilItExpires()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        const auto base = fixture.server.handler;
        auto limitedFor = [base](const QString &seconds) {
            return [base, seconds](const Request &request) {
                if (request.method == "GET" && request.target.path().endsWith("/streams/")) {
                    Response limited{QJsonArray{}, 429};
                    limited.headers.insert("Retry-After", seconds.toLatin1());
                    return limited;
                }
                return base(request);
            };
        };
        fixture.server.handler = limitedFor("1");
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("rate_limited"));
        QCOMPARE(fixture.controller.connectionSeverity(), QString("warning"));
        QVERIFY(fixture.controller.connectionRetryInSeconds() >= 1);
        // The countdown runs out even while the server answers again.
        fixture.server.handler = base;
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.connectionPhase(), QString("online"), 5000);
        // A generous Retry-After stays announced with its remaining seconds.
        fixture.server.handler = limitedFor("42");
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("rate_limited"));
        const auto remaining = fixture.controller.connectionRetryInSeconds();
        QVERIFY2(remaining > 30 && remaining <= 42, qPrintable(QString::number(remaining)));
    }

    void overlayBannerBootstrapFailureClearsAfterRetrySucceeds()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        QVERIFY(!fixture.controller.bootstrapError());
        const auto base = fixture.server.handler;
        // A malformed catalog row is a bootstrap failure: the snapshot is
        // cancelled and reported (report -> cancelSnapshot -> snapshotFailed).
        fixture.server.handler = [base](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", QStringLiteral("not-a-uuid")}}}};
            return base(request);
        };
        fixture.controller.refresh();
        QTRY_VERIFY(fixture.controller.bootstrapError());
        QVERIFY(!fixture.controller.error().isEmpty());
        fixture.server.handler = base;
        fixture.controller.retryBootstrap();
        QTRY_VERIFY(!fixture.controller.bootstrapError());
        QVERIFY(fixture.controller.error().isEmpty());
        QTRY_COMPARE(fixture.controller.streams().size(), 1);
    }

    void overlayBannerRealtimeDropShowsReconnectingAndReconnectClears()
    {
        Fixture fixture; RealtimeGateway gateway(fixture.server.serverPort());
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), QString("live"));
        QCOMPARE(fixture.controller.connectionPhase(), QString("online"));
        gateway.sockets.first()->close();
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("reconnecting"));
        // The gateway accepts the reconnect again and the banner clears itself.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.connectionPhase(), QString("online"), 15000);
    }

    // exo-review finding 1: a server that never answers must mark the
    // connection offline when the client request timeout fires.
    void overlayBannerRequestTimeoutMarksConnectionOffline()
    {
        // Handler captures must outlive the Fixture and every deferred send.
        QVector<std::function<void()>> held;
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.connectionPhase(), QString("online"));
        // An outage window that never answers: the requests are neither
        // completed nor refused, so the client timeout fires without any
        // HTTP response - a transport-level failure for connection health.
        fixture.server.handler = [&held](const Request &) {
            Response hold{QJsonArray{}};
            hold.defer = [&held](std::function<void()> send) { held.append(std::move(send)); };
            return hold;
        };
        fixture.controller.refresh();
        // ApiClient::RequestTimeoutMs is 30 s; wait out the client timer.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.connectionPhase(), QString("offline"), 35000);
        QCOMPARE(fixture.controller.connectionSeverity(), QString("critical"));
        held.clear(); // never released: teardown aborts the pending requests
    }

    // exo-review finding 2: a later 429 with a shorter Retry-After must not
    // shorten an already announced rate-limit countdown.
    void overlayBannerRateLimitKeepsTheLaterRetryDeadline()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        const auto base = fixture.server.handler;
        auto limitedFor = [base](const QString &seconds) {
            return [base, seconds](const Request &request) {
                if (request.method == "GET" && request.target.path().endsWith("/streams/")) {
                    Response limited{QJsonArray{}, 429};
                    limited.headers.insert("Retry-After", seconds.toLatin1());
                    return limited;
                }
                return base(request);
            };
        };
        fixture.server.handler = limitedFor("60");
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("rate_limited"));
        const auto announced = fixture.controller.connectionRetryInSeconds();
        QVERIFY2(announced > 50, qPrintable(QString::number(announced)));
        // A racing response with a shorter Retry-After arrives afterwards.
        QSignalSpy snapshotFailed(&fixture.controller, &NativeController::snapshotFailed);
        QVERIFY(snapshotFailed.isValid());
        fixture.server.handler = limitedFor("5");
        fixture.controller.refresh();
        // The callback that fails the snapshot runs after the transport
        // observer has already seen this second 429.
        QVERIFY(snapshotFailed.wait(5000));
        const auto remaining = fixture.controller.connectionRetryInSeconds();
        QVERIFY2(remaining > 30 && remaining <= announced, qPrintable(QString::number(remaining)));
    }

    // exo-review finding 3: a successful realtime reconnect must clear a
    // stale transport-offline flag even when no JSON response was observed.
    void overlayBannerRealtimeReconnectClearsTransportOffline()
    {
        // Handler captures must outlive the Fixture and every deferred send.
        QVector<std::function<void()>> heldCatchUps;
        auto holdNextCatchUp = std::make_shared<bool>(false);
        Fixture fixture; fixture.extraTopic = true;
        RealtimeGateway gateway(fixture.server.serverPort());
        fixture.realtime = [holdNextCatchUp, &heldCatchUps](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            Response response{QJsonArray{}};
            if (*holdNextCatchUp) {
                *holdNextCatchUp = false;
                response.defer = [&heldCatchUps](std::function<void()> send) { heldCatchUps.append(std::move(send)); };
            }
            return response;
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), QString("live"));
        QCOMPARE(fixture.controller.connectionPhase(), QString("online"));
        // Stall the reconnect catch-up so the websocket cannot report live
        // before the transport outage below has been observed by the client.
        *holdNextCatchUp = true;
        gateway.sockets.first()->close();
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("reconnecting"));
        QTRY_VERIFY(!heldCatchUps.isEmpty());
        // A controller request that dies at the transport level marks the
        // connection offline while realtime is still down.
        fixture.messages = [](const Request &) {
            Response dropped; dropped.abortConnection = true; return dropped;
        };
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("offline"));
        // The websocket reconnect completes without observing any JSON
        // response; a healthy realtime link must not leave the offline
        // banner set on top of live sync state.
        const auto catchUps = heldCatchUps;
        heldCatchUps.clear();
        for (const auto &send : catchUps)
            send();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.syncState(), QString("live"), 15000);
        QTRY_COMPARE(fixture.controller.connectionPhase(), QString("online"));
    }

    // --- Issue #144: top bar workspace session switcher -------------------
    // Reference: packages/web/src/widgets/top-bar/
    // top-bar-workspace-session-switcher.ui.tsx and
    // top-bar-workspace-session-unread.lib.ts at 61dacef7.

    void sessionSwitcherBadgeLabelUsesTheDisplayLimit()
    {
        // shared/lib/count-format.lib.ts:1 UNREAD_COUNT_DISPLAY_LIMIT = 1000;
        // the issue acceptance pins the cap label "1000+" for the native badge.
        QCOMPARE(NativeController::unreadBadgeLabel(0), QString("0"));
        QCOMPARE(NativeController::unreadBadgeLabel(37), QString("37"));
        QCOMPARE(NativeController::unreadBadgeLabel(1000), QString("1000"));
        QCOMPARE(NativeController::unreadBadgeLabel(1001), QString("1000+"));
        QCOMPARE(NativeController::unreadBadgeLabel(1500), QString("1000+"));
    }

    void sessionSwitcherProjectionUnreadFollowsTheReferenceLadder()
    {
        const QString all{"00000000-0000-0000-0000-000000000000"};
        const QString messageUuid{"55555555-5555-4555-8555-555555555555"};
        const auto candidate = [&messageUuid](const QString &stream, bool own = false, bool read = false) {
            return QVariantMap{{"messageUuid", messageUuid}, {"streamUuid", stream},
                {"isOwn", own}, {"read", read}};
        };
        // No projection yet -> no dot (unread lib :24).
        QCOMPARE(NativeController::backgroundProjectionUnread({}), 0);
        QCOMPARE(NativeController::backgroundProjectionUnread(
            QVariantMap{{"unreadByFolderId", QVariantMap{{all, 37}}}}), 37);
        // All-folder count is used as-is, clamped at zero (:26-29).
        QCOMPARE(NativeController::backgroundProjectionUnread(
            QVariantMap{{"unreadByFolderId", QVariantMap{{all, -5}, {"other", 9}}}}), 0);
        // Items placed in the all-chats folder sum before any other ladder (:31-39).
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"unreadByFolderItemId", QVariantMap{{"item-a", 4}, {"item-b", 3}, {"item-c", 9}}},
            {"folderItemTopologyById", QVariantMap{{"item-a", QVariantMap{{"folderUuid", all}}},
                {"item-b", QVariantMap{{"folderUuid", all}}}, {"item-c", QVariantMap{{"folderUuid", "f"}}}}}}), 7);
        // Folder sums (:41-43) and folder-item sums (:45-47), negatives clamped.
        QCOMPARE(NativeController::backgroundProjectionUnread(
            QVariantMap{{"unreadByFolderId", QVariantMap{{"f1", 2}, {"f2", -3}, {"f3", 5}}}}), 7);
        QCOMPARE(NativeController::backgroundProjectionUnread(
            QVariantMap{{"unreadByFolderItemId", QVariantMap{{"i1", 2}, {"i2", 3}}}}), 5);
        // Stream sums prefer activeUnreadCount and skip archived streams (:49-58).
        const auto streams = QVariantMap{
            {"s1", QVariantMap{{"streamUuid", "s1"}, {"unreadCount", 10}, {"activeUnreadCount", 4}}},
            {"s2", QVariantMap{{"streamUuid", "s2"}, {"unreadCount", 7}, {"isArchived", true}}},
            {"s3", QVariantMap{{"streamUuid", "s3"}, {"unreadCount", 2}, {"activeUnreadCount", 0}}}};
        QCOMPARE(NativeController::backgroundProjectionUnread(
            QVariantMap{{"streamSnapshotsById", streams}}), 4);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"streamSnapshotsById", streams},
            {"notificationCandidates", QVariantList{candidate("s9")}}}), 5);
        // A candidate on an unknown stream counts only while active (:60-65).
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"streamSnapshotsById", streams},
            {"notificationCandidates", QVariantList{candidate("s9", false, true)}}}), 4);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"streamSnapshotsById", streams},
            {"notificationCandidates", QVariantList{candidate("s9", true)}}}), 4);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"streamSnapshotsById", streams},
            {"notificationCandidates", QVariantList{candidate("s1")}}}), 4);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"streamSnapshotsById", streams},
            {"notificationCandidates", QVariantList{candidate("s9")}},
            {"messageIdSnapshotsById", QVariantMap{{messageUuid, QVariantMap{{"read", true}}}}}}), 4);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"streamSnapshotsById", streams},
            {"notificationCandidates", QVariantList{candidate("s9")}},
            {"messageIdSnapshotsById", QVariantMap{{messageUuid,
                QVariantMap{{"deletedAt", "2026-01-02T10:00:00Z"}}}}}}), 4);
        // Legacy candidate presence fallback (:68-71).
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"notificationCandidates", QVariantList{candidate("s1")}}}), 1);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"notificationCandidates", QVariantList{candidate("s1", false, true)}}}), 0);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"notificationCandidates", QVariantList{candidate("s1", true)}}}), 0);
        QCOMPARE(NativeController::backgroundProjectionUnread(QVariantMap{
            {"notificationCandidates", QVariantList{candidate("s1")}},
            {"messageIdSnapshotsById", QVariantMap{{messageUuid, QVariantMap{{"read", true}}}}}}), 0);
    }

    void sessionSwitcherRowsProjectRememberedAccounts()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const QUrl server("https://switcher.invalid");
        const QString otherUser{"66666666-6666-4666-8666-666666666666"};
        const QString otherProject{"77777777-7777-4777-8777-777777777777"};
        SavedAccount alice{SessionVault::accountId(server, projectId, userId), server,
            projectId, userId, "alice", "Alpha project"};
        SavedAccount anonymous{SessionVault::accountId(server, otherProject, otherUser),
            server, otherProject, otherUser, "   ", "Beta project"};
        bool savedAlice = false;
        bool savedAnonymous = false;
        vault.save(alice, "synthetic-refresh-a", [&](const QString &error) { QVERIFY(error.isEmpty()); savedAlice = true; });
        vault.save(anonymous, "synthetic-refresh-b", [&](const QString &error) { QVERIFY(error.isEmpty()); savedAnonymous = true; });
        QTRY_VERIFY(savedAlice && savedAnonymous);
        ApiClient api; Session session(&api); session.setVault(&vault);
        NativeController controller(&api, &session);

        const auto rows = controller.sessionSwitcherSessions();
        QCOMPARE(rows.size(), 2);
        const auto rowFor = [&rows](const QString &id) {
            for (const auto &row : rows)
                if (row.toMap().value("id").toString() == id) return row.toMap();
            return QVariantMap{};
        };
        const auto aliceRow = rowFor(alice.id);
        QCOMPARE(aliceRow.value("primaryLabel").toString(), QString("alice"));
        QCOMPARE(aliceRow.value("secondaryLabel").toString(),
            QStringLiteral("switcher.invalid \xC2\xB7 ") + projectId);
        QCOMPARE(aliceRow.value("title").toString(),
            QStringLiteral("alice \xC2\xB7 switcher.invalid \xC2\xB7 ") + projectId);
        QCOMPARE(aliceRow.value("unread").toInt(), 0);   // no projection -> no dot
        QCOMPARE(aliceRow.value("isCurrent").toBool(), false);
        // The blank login falls back to the user uuid (switcher :40-43).
        QCOMPARE(rowFor(anonymous.id).value("primaryLabel").toString(), otherUser);
        // Per-account projections feed the badges (unread lib ladder shared).
        controller.publishSessionProjection(alice.id,
            QVariantMap{{"unreadByFolderId", QVariantMap{{QStringLiteral("00000000-0000-0000-0000-000000000000"), 37}}}});
        const auto projected = controller.sessionSwitcherSessions();
        const auto projectedAlice = [&]{ for (const auto &row : projected)
            if (row.toMap().value("id").toString() == alice.id) return row.toMap(); return QVariantMap{}; }();
        QCOMPARE(projectedAlice.value("unread").toInt(), 37);
        controller.publishSessionProjection(alice.id, {});
        const auto cleared = controller.sessionSwitcherSessions();
        const auto clearedAlice = [&]{ for (const auto &row : cleared)
            if (row.toMap().value("id").toString() == alice.id) return row.toMap(); return QVariantMap{}; }();
        QCOMPARE(clearedAlice.value("unread").toInt(), 0);
    }

    void sessionSwitcherSelectDetachAndAddServerTransitions()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const QUrl server("https://switcher.invalid");
        const QString userB{"66666666-6666-4666-8666-666666666666"};
        const QString projectB{"77777777-7777-4777-8777-777777777777"};
        SavedAccount a{SessionVault::accountId(server, projectId, userId), server, projectId, userId, "alice", "Alpha"};
        SavedAccount b{SessionVault::accountId(server, projectB, userB), server, projectB, userB, "bob", "Beta"};
        bool savedA = false;
        bool savedB = false;
        vault.save(a, "switch-refresh-a", [&](const QString &error) { QVERIFY(error.isEmpty()); savedA = true; });
        vault.save(b, "switch-refresh-b", [&](const QString &error) { QVERIFY(error.isEmpty()); savedB = true; });
        QTRY_VERIFY(savedA && savedB);
        auto transport = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        auto current = std::make_shared<QPair<QString, QString>>();
        transport->insert("switch-refresh-a", {userId, projectId});
        transport->insert("switch-refresh-b", {userB, projectB});
        ApiClient api;
        Session session(&api, nullptr, [transport, current, &api](const QByteArray &, const QString &path,
            const QJsonValue &body, const QUrlQuery &, ApiClient::Callback done, bool,
            const QMap<QByteArray, QByteArray> &) {
            QJsonValue data; int status = 404; QString error{"not found"};
            if (path.endsWith("/actions/get_token/invoke")) {
                *current = transport->value(body.toObject().value("refresh_token").toString());
                data = QJsonObject{{"access_token", "synthetic-token"}, {"refresh_token",
                    body.toObject().value("refresh_token").toString()}, {"expires_in", 3600}};
                status = 200; error.clear();
            } else if (path.endsWith("/actions/introspect")) {
                data = QJsonObject{{"user_info", QJsonObject{{"uuid", current->first}}},
                    {"project_id", current->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                status = 200; error.clear();
            } else if (path == "/api/core/v1/iam/projects/") {
                data = QJsonArray{QJsonObject{{"uuid", current->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                status = 200; error.clear();
            }
            const ApiResult result{data, status, {}, error};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);

        controller.resumeSavedAccount(a.id);
        QTRY_VERIFY(controller.authenticated());
        {
            const auto rows = controller.sessionSwitcherSessions();
            QCOMPARE(rows.first().toMap().value("isCurrent").toBool(), true);
            QCOMPARE(rows.last().toMap().value("isCurrent").toBool(), false);
        }
        // Selecting the current session changes nothing.
        controller.selectWorkspaceSession(a.id);
        QVERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        // Switching keeps every remembered identity (reference behaviour).
        controller.selectWorkspaceSession(b.id);
        QTRY_VERIFY(controller.authenticated());
        QTRY_COMPARE(controller.sessionSwitcherSessions().first().toMap().value("isCurrent"), QVariant(true));
        QCOMPARE(controller.savedAccounts().size(), 2);
        QCOMPARE(secrets.values.size(), 2);
        // "Add server" opens the sign-in step with remembered sessions intact.
        QTRY_VERIFY(!controller.busy());
        controller.openAddServer();
        QTRY_VERIFY(!controller.authenticated());
        QCOMPARE(controller.loginStep(), QString("organization"));
        QCOMPARE(controller.savedAccounts().size(), 2);
    }

    void sessionTransitionsRetireNotificationOwner_data()
    {
        QTest::addColumn<QString>("transition");
        QTest::addColumn<bool>("remembered");
        QTest::addColumn<bool>("successor");
        QTest::addColumn<QString>("observation");
        for (const auto &observation : {"banners", "pending", "actions"}) {
            for (const bool remembered : {false, true}) {
                for (const bool successor : {false, true})
                    QTest::addRow("switcher-%s-%s-%s", remembered ? "remembered" : "live",
                        successor ? "successor" : "last", observation)
                        << QString("switcher") << remembered << successor << QString(observation);
                QTest::addRow("ordinary-%s-%s", remembered ? "remembered" : "live", observation)
                    << QString("ordinary") << remembered << true << QString(observation);
            }
            for (const auto &transition : {"switch", "add-server", "refresh", "remove-other"})
                QTest::addRow("%s-%s", transition, observation)
                    << QString(transition) << true << true << QString(observation);
        }
    }

    void sessionTransitionsRetireNotificationOwner()
    {
        QFETCH(QString, transition); QFETCH(bool, remembered); QFETCH(bool, successor);
        QFETCH(QString, observation);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("notification-accounts.ini"), &secrets);
        const QUrl server("https://switcher.invalid");
        const QString userB{"66666666-6666-4666-8666-666666666666"};
        SavedAccount a{SessionVault::accountId(server, projectId, userId), server, projectId, userId, "alice", "Alpha"};
        SavedAccount b{SessionVault::accountId(server, projectId, userB), server, projectId, userB, "bob", "Beta"};
        int saves = 0;
        if (remembered)
            vault.save(a, "switch-refresh-a", [&](const QString &error) { QVERIFY(error.isEmpty()); ++saves; });
        if (successor)
            vault.save(b, "switch-refresh-b", [&](const QString &error) { QVERIFY(error.isEmpty()); ++saves; });
        QTRY_COMPARE(saves, int(remembered) + int(successor));
        QString currentUser = userId;
        ApiClient api;
        Session session(&api, nullptr, [&](const QByteArray &, const QString &path,
            const QJsonValue &body, const QUrlQuery &, ApiClient::Callback done, bool,
            const QMap<QByteArray, QByteArray> &) {
            QJsonValue data; int status = 404; QString error{"not found"};
            if (path.endsWith("/actions/get_token/invoke")) {
                currentUser = body.toObject().value("refresh_token").toString() == "switch-refresh-b" ? userB : userId;
                data = QJsonObject{{"access_token", "synthetic-token"}, {"refresh_token",
                    currentUser == userB ? "switch-refresh-b" : "switch-refresh-a"}, {"expires_in", 3600}};
                status = 200; error.clear();
            } else if (path.endsWith("/actions/introspect")) {
                data = QJsonObject{{"user_info", QJsonObject{{"uuid", currentUser}}},
                    {"project_id", projectId}, {"otp_verified", true}, {"permissions", QJsonArray{}}};
                status = 200; error.clear();
            } else if (path == "/api/core/v1/iam/projects/") {
                data = QJsonArray{QJsonObject{{"uuid", projectId}, {"name", "Notification project"}, {"status", "ACTIVE"}}};
                status = 200; error.clear();
            }
            const ApiResult result{data, status, {}, error};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        NativeController controller(&api, &session);
        controller.setRealtimeMode(true);
        if (remembered) {
            session.setVault(&vault);
            controller.resumeSavedAccount(a.id);
        } else {
            controller.login(server.toString(), "alice", "synthetic-password", "");
            QTRY_COMPARE(controller.projects().size(), 1);
            controller.openProject(projectId);
        }
        QTRY_VERIFY(controller.authenticated()); QTRY_VERIFY(!controller.busy());
        if (!remembered) session.setVault(&vault); // A stays a synthesized live row; B may be remembered.
        QCOMPARE(session.currentSavedAccountId(), remembered ? a.id : QString{});
        QCOMPARE(controller.sessionSwitcherSessions().first().toMap().value("id").toString(), a.id);
        QCOMPARE(controller.sessionSwitcherSessions().first().toMap().value("isCurrent").toBool(), true);
        const auto ownerA = controller.creationOwnerKey(); QVERIFY(!ownerA.isEmpty());
        RecordingNotifications sent;
        auto *service = controller.notifications(); service->setTransport(&sent);
        const auto resetTransport = qScopeGuard([&] { service->setTransport(nullptr); });
        int retiredMetadataLookups = 0;
        service->setMetadataResolver([&](NotificationCandidate &candidate) {
            if (candidate.owner != controller.creationOwnerKey()) ++retiredMetadataLookups;
            return candidate.messageUuid == historyMessage(500).value("uuid").toString();
        });
        NotificationCandidate candidate;
        candidate.owner = ownerA; candidate.messageUuid = historyMessage(500).value("uuid").toString();
        candidate.streamUuid = streamId; candidate.topicUuid = topicId;
        candidate.authorUuid = notificationAuthor; candidate.content = "Synthetic notification";
        candidate.streamMode = "all_messages"; candidate.streamKnown = true; candidate.topicKnown = true;
        candidate.streamName = "Channel"; candidate.topicName = "Topic"; candidate.senderName = "Author";
        service->ingest(candidate);
        QCOMPARE(sent.shows.size(), 1);
        const auto aggregate = sent.lastTag(); QVERIFY(aggregate.startsWith("bucket:"));
        service->setMetadataGrace(30, 5);
        candidate.messageUuid = historyMessage(501).value("uuid").toString();
        service->ingest(candidate);
        QTRY_COMPARE(sent.shows.size(), 2);
        const auto fallback = sent.lastTag(); QVERIFY(fallback.startsWith("msg:"));
        service->setMetadataGrace(250, 5);
        candidate.messageUuid = historyMessage(502).value("uuid").toString();
        service->ingest(candidate);
        QCOMPARE(service->pendingCount(), 1);
        QSignalSpy routed(service, &NotificationService::routeRequested);
        QSignalSpy activated(service, &NotificationService::activationRequested);
        if (transition == "switcher") controller.logoutWorkspaceSession(a.id);
        else if (transition == "ordinary") controller.logout();
        else if (transition == "switch") controller.selectWorkspaceSession(b.id);
        else if (transition == "add-server") controller.openAddServer();
        else if (transition == "refresh") session.requestRefresh();
        else controller.logoutWorkspaceSession(b.id);
        const bool keepsOwner = transition == "refresh" || transition == "remove-other";
        if (transition == "switch" || (transition == "switcher" && successor)) {
            QTRY_VERIFY(controller.authenticated()); QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(session.user(), userB);
            QVERIFY(controller.creationOwnerKey() != ownerA);
        } else if (keepsOwner) {
            QTRY_VERIFY(!controller.busy()); QVERIFY(controller.authenticated());
            QCOMPARE(controller.creationOwnerKey(), ownerA);
        } else {
            QTRY_VERIFY(!controller.authenticated());
        }
        if (observation == "banners") {
            QCOMPARE(sent.closed.contains(aggregate), !keepsOwner);
            QCOMPARE(sent.closed.contains(fallback), !keepsOwner);
            QCOMPARE(service->trackedTagCount(), keepsOwner ? 1 : 0);
        } else if (observation == "pending") {
            QTest::qWait(350); // Longer than the real shortened grace timer.
            QCOMPARE(sent.shows.size(), keepsOwner ? 3 : 2);
            QCOMPARE(service->pendingCount(), 0);
            QCOMPARE(retiredMetadataLookups, 0);
        } else {
            service->click(aggregate); service->click(fallback);
            QCOMPARE(routed.size(), keepsOwner ? 2 : 0);
            QCOMPARE(activated.size(), keepsOwner ? 2 : 0);
            if (!keepsOwner) {
                QVERIFY(controller.selectedStream().isEmpty());
                QVERIFY(controller.selectedTopic().isEmpty());
            }
        }
    }

    void sessionSwitcherLogoutRoutesThroughNextInbox()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const QUrl server("https://switcher.invalid");
        const QString userB{"66666666-6666-4666-8666-666666666666"};
        const QString projectB{"77777777-7777-4777-8777-777777777777"};
        const QString userC{"88888888-8888-4888-8888-888888888888"};
        const QString projectC{"99999999-9999-4999-9999-999999999999"};
        SavedAccount a{SessionVault::accountId(server, projectId, userId), server, projectId, userId, "alice", "Alpha"};
        SavedAccount b{SessionVault::accountId(server, projectB, userB), server, projectB, userB, "bob", "Beta"};
        SavedAccount c{SessionVault::accountId(server, projectC, userC), server, projectC, userC, "cara", "Gamma"};
        bool savedA = false;
        bool savedB = false;
        bool savedC = false;
        vault.save(a, "switch-refresh-a", [&](const QString &error) { QVERIFY(error.isEmpty()); savedA = true; });
        vault.save(b, "switch-refresh-b", [&](const QString &error) { QVERIFY(error.isEmpty()); savedB = true; });
        vault.save(c, "switch-refresh-c", [&](const QString &error) { QVERIFY(error.isEmpty()); savedC = true; });
        QTRY_VERIFY(savedA && savedB && savedC);
        auto transport = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        auto current = std::make_shared<QPair<QString, QString>>();
        transport->insert("switch-refresh-a", {userId, projectId});
        transport->insert("switch-refresh-b", {userB, projectB});
        transport->insert("switch-refresh-c", {userC, projectC});
        ApiClient api;
        Session session(&api, nullptr, [transport, current, &api](const QByteArray &, const QString &path,
            const QJsonValue &body, const QUrlQuery &, ApiClient::Callback done, bool,
            const QMap<QByteArray, QByteArray> &) {
            QJsonValue data; int status = 404; QString error{"not found"};
            if (path.endsWith("/actions/get_token/invoke")) {
                *current = transport->value(body.toObject().value("refresh_token").toString());
                data = QJsonObject{{"access_token", "synthetic-token"}, {"refresh_token",
                    body.toObject().value("refresh_token").toString()}, {"expires_in", 3600}};
                status = 200; error.clear();
            } else if (path.endsWith("/actions/introspect")) {
                data = QJsonObject{{"user_info", QJsonObject{{"uuid", current->first}}},
                    {"project_id", current->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                status = 200; error.clear();
            } else if (path == "/api/core/v1/iam/projects/") {
                data = QJsonArray{QJsonObject{{"uuid", current->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                status = 200; error.clear();
            }
            const ApiResult result{data, status, {}, error};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        controller.resumeSavedAccount(a.id);
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        controller.publishSessionProjection(a.id, QVariantMap{
            {"unreadByFolderId", QVariantMap{{QStringLiteral("00000000-0000-0000-0000-000000000000"), 5}}}});
        // Removing a non-current remembered account keeps the live session.
        controller.logoutWorkspaceSession(c.id);
        QVERIFY(controller.authenticated());
        QTRY_COMPARE(controller.savedAccounts().size(), 2);
        // The projection registry forgets removed accounts.
        controller.publishSessionProjection(c.id, QVariantMap{
            {"unreadByFolderId", QVariantMap{{QStringLiteral("00000000-0000-0000-0000-000000000000"), 9}}}});
        controller.forgetSavedAccount(c.id);
        QVERIFY(controller.authenticated());
        // Logging out of the current session removes it and activates the next
        // remembered inbox (workspace-post-logout-route.lib.ts).
        QTRY_VERIFY(!controller.busy());
        controller.logoutWorkspaceSession(a.id);
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.savedAccounts().size(), 1);
        QTRY_COMPARE(controller.sessionSwitcherSessions().first().toMap().value("id").toString(), b.id);
        QCOMPARE(controller.sessionSwitcherSessions().first().toMap().value("isCurrent").toBool(), true);
        QVERIFY(!secrets.values.contains(a.id));
        // Removing the last remembered session leaves the sign-in step.
        controller.logoutWorkspaceSession(b.id);
        QTRY_VERIFY(!controller.authenticated());
        QCOMPARE(controller.savedAccounts().size(), 0);
        QVERIFY(secrets.values.isEmpty());
    }

    void sessionSwitcherLogoutReportsFailedCredentialRemoval()
    {
        // exo-review #170: resuming the successor must not swallow a failed
        // secure-store removal of the logged-out credential. The successor's
        // attempt may no longer discard the removal error, and the error has
        // to reach the user before the successor finishes taking over.
        QTemporaryDir directory; QVERIFY(directory.isValid());
        ControllerFailingRemovalSecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const QUrl server("https://switcher.invalid");
        const QString userB{"66666666-6666-4666-8666-666666666666"};
        const QString projectB{"77777777-7777-4777-8777-777777777777"};
        SavedAccount a{SessionVault::accountId(server, projectId, userId), server, projectId, userId, "alice", "Alpha"};
        SavedAccount b{SessionVault::accountId(server, projectB, userB), server, projectB, userB, "bob", "Beta"};
        bool savedA = false;
        bool savedB = false;
        vault.save(a, "switch-refresh-a", [&](const QString &error) { QVERIFY(error.isEmpty()); savedA = true; });
        vault.save(b, "switch-refresh-b", [&](const QString &error) { QVERIFY(error.isEmpty()); savedB = true; });
        QTRY_VERIFY(savedA && savedB);
        auto transport = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        auto current = std::make_shared<QPair<QString, QString>>();
        transport->insert("switch-refresh-a", {userId, projectId});
        transport->insert("switch-refresh-b", {userB, projectB});
        ApiClient api;
        Session session(&api, nullptr, [transport, current, &api](const QByteArray &, const QString &path,
            const QJsonValue &body, const QUrlQuery &, ApiClient::Callback done, bool,
            const QMap<QByteArray, QByteArray> &) {
            QJsonValue data; int status = 404; QString error{"not found"};
            if (path.endsWith("/actions/get_token/invoke")) {
                *current = transport->value(body.toObject().value("refresh_token").toString());
                data = QJsonObject{{"access_token", "synthetic-token"}, {"refresh_token",
                    body.toObject().value("refresh_token").toString()}, {"expires_in", 3600}};
                status = 200; error.clear();
            } else if (path.endsWith("/actions/introspect")) {
                data = QJsonObject{{"user_info", QJsonObject{{"uuid", current->first}}},
                    {"project_id", current->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                status = 200; error.clear();
            } else if (path == "/api/core/v1/iam/projects/") {
                data = QJsonArray{QJsonObject{{"uuid", current->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                status = 200; error.clear();
            }
            const ApiResult result{data, status, {}, error};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        controller.resumeSavedAccount(a.id);
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        bool removalErrorSeen = false;
        bool removalBeforeSuccessor = false;
        bool loggedOut = false;
        bool successorReady = false;
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (!removalErrorSeen && controller.error().contains("could not be removed")) {
                removalErrorSeen = true;
                removalBeforeSuccessor = !successorReady;
            }
            if (!loggedOut && !controller.authenticated()) loggedOut = true;
            if (loggedOut && !successorReady && controller.authenticated() && !controller.busy())
                successorReady = true;
        });
        controller.logoutWorkspaceSession(a.id);
        // The successor promise still holds: the next remembered inbox wins.
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(successorReady);
        QCOMPARE(controller.savedAccounts().size(), 1);
        QCOMPARE(controller.sessionSwitcherSessions().first().toMap().value("id").toString(), b.id);
        // The failed vault removal must surface, not be discarded by the
        // successor's advanced attempt, and it must precede the handover.
        QVERIFY2(removalErrorSeen, qPrintable(QStringLiteral("error=%1").arg(controller.error())));
        QVERIFY(removalBeforeSuccessor);
        // The credential genuinely survived the failed removal - which is
        // exactly why the user was told about it.
        QVERIFY(secrets.values.contains(a.id));
        QVERIFY(secrets.values.contains(b.id));
    }

    void sessionSwitcherLogoutClearsSynthesizedLiveSession()
    {
        // exo-review #170: while the live session has no saved-account
        // binding, the switcher renders a row with a synthesized id. Logging
        // out that row must clear the live session, not try to forget a row
        // that was never remembered.
        const QUrl server("https://switcher.invalid");
        ApiClient api;
        Session session(&api, nullptr, [&api](const QByteArray &, const QString &path,
            const QJsonValue &body, const QUrlQuery &, ApiClient::Callback done, bool,
            const QMap<QByteArray, QByteArray> &) {
            QJsonValue data; int status = 404; QString error{"not found"};
            const auto grant = body.toObject().value("grant_type").toString();
            if (path.endsWith("/actions/get_token/invoke")
                && (grant == "login+password" || grant == "refresh_token")) {
                data = QJsonObject{{"access_token", "synthetic-token"},
                    {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}};
                status = 200; error.clear();
            } else if (path.endsWith("/actions/introspect")) {
                data = QJsonObject{{"user_info", QJsonObject{{"uuid", userId}}},
                    {"project_id", projectId}, {"otp_verified", true}, {"permissions", QJsonArray{}}};
                status = 200; error.clear();
            } else if (path == "/api/core/v1/iam/projects/") {
                data = QJsonArray{QJsonObject{{"uuid", projectId},
                    {"name", "Live project"}, {"status", "ACTIVE"}}};
                status = 200; error.clear();
            }
            const ApiResult result{data, status, {}, error};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        NativeController controller(&api, &session);   // No vault: the live identity stays unsaved.
        controller.login(server.toString(), "alice", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QVERIFY(session.currentSavedAccountId().isEmpty());
        const auto rows = controller.sessionSwitcherSessions();
        QCOMPARE(rows.size(), 1);
        const auto liveId = SessionVault::accountId(server, projectId, userId);
        QVERIFY(!liveId.isEmpty());
        QCOMPARE(rows.first().toMap().value("id").toString(), liveId);
        QCOMPARE(rows.first().toMap().value("isCurrent").toBool(), true);
        // Logging out the synthesized live row clears the live session and
        // lands back on the sign-in step instead of "forgetting" an unknown row.
        controller.logoutWorkspaceSession(liveId);
        QTRY_VERIFY(!controller.authenticated());
        QCOMPARE(controller.loginStep(), QString("organization"));
        QVERIFY(controller.error().isEmpty());
    }

    void sessionSwitcherDelayedLogoutKeepsNewerAccountChoice()
    {
        // exo-review #170 round 2: a logout whose secure-store removal stays
        // in flight must not override a session the user picked meanwhile.
        // The successor handover is captured at logout time; releasing the
        // held removal after the user selected another account must leave
        // the newer choice in charge.
        QTemporaryDir directory; QVERIFY(directory.isValid());
        ControllerHeldRemovalSecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const QUrl server("https://switcher.invalid");
        const QString userB{"66666666-6666-4666-8666-666666666666"};
        const QString projectB{"77777777-7777-4777-8777-777777777777"};
        const QString userC{"88888888-8888-4888-8888-888888888888"};
        const QString projectC{"99999999-9999-4999-9999-999999999999"};
        SavedAccount a{SessionVault::accountId(server, projectId, userId), server, projectId, userId, "alice", "Alpha"};
        SavedAccount b{SessionVault::accountId(server, projectB, userB), server, projectB, userB, "bob", "Beta"};
        SavedAccount c{SessionVault::accountId(server, projectC, userC), server, projectC, userC, "cara", "Gamma"};
        bool savedA = false;
        bool savedB = false;
        bool savedC = false;
        vault.save(a, "switch-refresh-a", [&](const QString &error) { QVERIFY(error.isEmpty()); savedA = true; });
        vault.save(b, "switch-refresh-b", [&](const QString &error) { QVERIFY(error.isEmpty()); savedB = true; });
        vault.save(c, "switch-refresh-c", [&](const QString &error) { QVERIFY(error.isEmpty()); savedC = true; });
        QTRY_VERIFY(savedA && savedB && savedC);
        auto transport = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        auto current = std::make_shared<QPair<QString, QString>>();
        transport->insert("switch-refresh-a", {userId, projectId});
        transport->insert("switch-refresh-b", {userB, projectB});
        transport->insert("switch-refresh-c", {userC, projectC});
        ApiClient api;
        Session session(&api, nullptr, [transport, current, &api](const QByteArray &, const QString &path,
            const QJsonValue &body, const QUrlQuery &, ApiClient::Callback done, bool,
            const QMap<QByteArray, QByteArray> &) {
            QJsonValue data; int status = 404; QString error{"not found"};
            if (path.endsWith("/actions/get_token/invoke")) {
                *current = transport->value(body.toObject().value("refresh_token").toString());
                data = QJsonObject{{"access_token", "synthetic-token"}, {"refresh_token",
                    body.toObject().value("refresh_token").toString()}, {"expires_in", 3600}};
                status = 200; error.clear();
            } else if (path.endsWith("/actions/introspect")) {
                data = QJsonObject{{"user_info", QJsonObject{{"uuid", current->first}}},
                    {"project_id", current->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                status = 200; error.clear();
            } else if (path == "/api/core/v1/iam/projects/") {
                data = QJsonArray{QJsonObject{{"uuid", current->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                status = 200; error.clear();
            }
            const ApiResult result{data, status, {}, error};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        controller.resumeSavedAccount(a.id);
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        // Log out of the current account; the secure-store removal is held.
        controller.logoutWorkspaceSession(a.id);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ return secrets.releaseRemove != nullptr; })(), 5000);
        QTRY_VERIFY(!controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        // While the removal is pending, the user picks another remembered
        // account. The session binds the newer choice immediately, even while
        // the vault queue parks its load behind the pending removal.
        controller.selectWorkspaceSession(c.id);
        QCOMPARE(session.currentSavedAccountId(), c.id);
        // The removal completes late; its captured successor must not win.
        // The vault completion runs synchronously inside release(), so the
        // session binding right afterwards already shows the overwrite.
        const auto release = std::exchange(secrets.releaseRemove, {});
        QVERIFY(release);
        release();
        QCOMPARE(session.currentSavedAccountId(), c.id);
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ const auto rows = controller.sessionSwitcherSessions();
            return !rows.isEmpty() && rows.first().toMap().value("id").toString() == c.id
                && rows.first().toMap().value("isCurrent").toBool(); })(), 10000);
        // The released removal did land; the untouched accounts survive.
        QVERIFY(!secrets.values.contains(a.id));
        QVERIFY(secrets.values.contains(b.id));
        QVERIFY(secrets.values.contains(c.id));
    }

    // Acceptance (6): the auth idle guard is inactive before login, armed for
    // the live session, and cancelled by a manual sign-out.
    void authIdleGuardFollowsTheLiveSession()
    {
        Fixture fixture;
        auto &c = fixture.controller;
        QSignalSpy changed(&c, &NativeController::authGuardActiveChanged);
        QVERIFY(!c.isAuthGuardActive()); // inactive before login
        QVERIFY(fixture.start());
        QVERIFY(c.isAuthGuardActive()); // armed for the live session
        QCOMPARE(changed.count(), 1);
        c.logout();
        QVERIFY(!c.isAuthGuardActive()); // cancelled by manual logout
        QCOMPARE(changed.count(), 2);
    }

    // Acceptance (2)/(6): an unattended deadline routes through the same
    // NativeController::logout as a manual sign-out: signedOut exactly once
    // with the same cleared session state, and the guard disarms with it.
    void authIdleGuardExpirySignsOutExactlyOnceLikeManualLogout()
    {
        Fixture fixture;
        auto &c = fixture.controller;
        QVERIFY(fixture.start());
        auto *guard = c.authGuard();
        QVERIFY(guard);
        guard->setWindowsVisible(true);
        guard->setPollIntervalMs(5);
        qint64 fake = 1'000'000'000LL;
        guard->setNowProvider([&fake] { return fake; });
        guard->setTimeoutPreset(QStringLiteral("6h"));
        QVERIFY(guard->active());
        QSignalSpy signedOut(&fixture.session, &Session::signedOut);
        fake += 21600000LL + 1; // the full idle window plus one millisecond
        QTRY_VERIFY_WITH_TIMEOUT(signedOut.count() == 1, 2000);
        // Same observable end state as a manual c.logout(): live credentials
        // are gone (Session::logout also wipes the secure store) and the
        // guard follows the signedOut signal down.
        QVERIFY(!c.authenticated());
        QVERIFY(c.account().isEmpty());
        QVERIFY(!c.isAuthGuardActive());
        fake += 7LL * 24 * 3600 * 1000;
        QTest::qWait(80);
        QCOMPARE(signedOut.count(), 1); // exactly once, never refires
    }

    // Live preset reaction: a preference change re-arms the running guard
    // without any restart, mirroring the layout hook effect dependency.
    void authIdleGuardReactsToPreferenceChangesLive()
    {
        Fixture fixture;
        auto &c = fixture.controller;
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        c.setPreferences(&preferences);
        QVERIFY(fixture.start());
        auto *guard = c.authGuard();
        QVERIFY(guard);
        QVERIFY(guard->active());
        const auto threeDayDeadline = guard->expiresAtMs();
        preferences.setAuthIdleTimeout(QStringLiteral("6h"));
        const auto sixHours = 21600000LL;
        const auto elapsed = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(qAbs(guard->expiresAtMs() - (elapsed + sixHours)) < 5000); // re-armed to 6h
        preferences.setAuthIdleTimeout(QStringLiteral("never"));
        QVERIFY(!guard->active()); // cancel-only, like the hook cleanup test
        QVERIFY(!c.isAuthGuardActive());
        QVERIFY(threeDayDeadline > 0);
    }

    // Regression (PR #226 review, ID 4555758295): detaching the preferences
    // object, changing the preset on it while detached (the change signal is
    // ignored then), and reattaching the very same object must re-read the
    // current preset - skipping init on reattach would leave the guard armed
    // with the stale deadline from before the detach.
    void authIdleGuardReadoptsPresetChangedWhilePreferencesDetached()
    {
        Fixture fixture;
        auto &c = fixture.controller;
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        c.setPreferences(&preferences);
        QVERIFY(fixture.start());
        auto *guard = c.authGuard();
        QVERIFY(guard);
        QVERIFY(guard->active());
        preferences.setAuthIdleTimeout(QStringLiteral("6h"));
        c.setPreferences(nullptr); // detach: signal changes from here on are missed
        preferences.setAuthIdleTimeout(QStringLiteral("24h")); // the missed change
        c.setPreferences(&preferences); // reattach the same object
        QVERIFY(guard->active());
        const auto twentyFourHours = 86400000LL;
        const auto elapsed = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(qAbs(guard->expiresAtMs() - (elapsed + twentyFourHours)) < 5000); // fresh 24h, not stale 6h
        // Live reaction still works after the reattachment, with exactly one
        // signal connection (no duplicate re-arm through the old binding).
        preferences.setAuthIdleTimeout(QStringLiteral("never"));
        QVERIFY(!guard->active());
        QVERIFY(!c.isAuthGuardActive());
    }
};

QTEST_MAIN(ControllerSessionsTest)
#include "native-controller-sessions-test.moc"
