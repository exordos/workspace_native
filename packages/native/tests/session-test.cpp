#include "mock-server.h"
#include "session.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace workspace;
using namespace workspace::test;

class SessionMemorySecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    void write(const QString &key, const QByteArray &secret, Completion done) override {
        values.insert(key, secret);
        QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void read(const QString &key, Loaded done) override {
        const auto value = values.value(key);
        QTimer::singleShot(0, this, [done, value] { done(value, value.isEmpty() ? Error::NotFound : Error::None); });
    }
    void remove(const QString &key, Completion done) override {
        values.remove(key);
        QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
};

struct HttpsSessionTransport {
    QObject context;
    QVector<Request> requests;
    QString identityUser = userId;
    QString identityProject = projectId;
    QString rotated = "synthetic-rotated-refresh";
    bool offline = false;
    bool rejectRefresh = false;
    Session::RequestExecutor executor() {
        return [this](const QByteArray &method, const QString &path, const QJsonValue &body,
                      const QUrlQuery &, ApiClient::Callback done, bool,
                      const QMap<QByteArray, QByteArray> &headers) {
            Request request{method, QUrl(path), {}, body.toObject()};
            for (auto it = headers.begin(); it != headers.end(); ++it) request.headers.insert(it.key(), it.value());
            requests.append(request);
            ApiResult result;
            result.status = offline ? 0 : 200;
            if (offline) result.error = "Synthetic offline";
            else if (rejectRefresh && request.body.value("grant_type") == "refresh_token") {
                result.status = 401;
                result.error = "Synthetic invalid grant";
                result.data = QJsonObject{{"error", "invalid_grant"}};
            } else if (path.endsWith("/get_token/invoke")) {
                result.data = QJsonObject{{"access_token", "synthetic-access"},
                    {"refresh_token", request.body.value("grant_type") == "refresh_token" ? rotated : "synthetic-initial-refresh"},
                    {"expires_in", 3600}};
            } else if (path.endsWith("/introspect")) {
                result.data = QJsonObject{{"user_info", QJsonObject{{"uuid", identityUser}}}, {"project_id", identityProject}};
            } else result.data = QJsonArray{QJsonObject{{"uuid", projectId}, {"name", "Synthetic project"}}};
            QTimer::singleShot(0, &context, [done, result] { done(result); });
        };
    }
};

static SavedAccount savedIdentity()
{
    SavedAccount saved;
    saved.server = QUrl("https://workspace.example");
    saved.project = projectId;
    saved.user = userId;
    saved.login = "synthetic-user";
    saved.projectName = "Synthetic project";
    saved.id = SessionVault::accountId(saved.server, saved.project, saved.user);
    return saved;
}

class SessionTest final : public QObject {
    Q_OBJECT
private slots:
    void publicProfileIsWhitelistedAndPublishedOnlyAfterVerifiedIdentity()
    {
        QJsonObject publicUser{{"uuid", userId}, {"username", "  synthetic-cassi  "},
            {"first_name", " Cassandra "}, {"last_name", " Volkova "},
            {"email", " cassi@example.invalid "}, {"name", "deprecated-login"},
            {"access_token", "synthetic-private-access"}, {"otp_secret", "synthetic-private-otp"},
            {"type", "admin"}, {"permissions", QJsonArray{"synthetic-private-permission"}},
            {"avatar_url", "https://example.invalid/arbitrary-avatar"}};
        MockServer server;
        server.handler = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", publicUser},
                    {"refresh_token", "synthetic-private-refresh"}};
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy ready(&session, &Session::authenticated);
        QVERIFY(session.publicProfile().isEmpty());
        session.login(server.origin(), "synthetic-login", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        QVERIFY(session.publicProfile().isEmpty());
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        const auto profile = session.publicProfile();
        QCOMPARE(profile.keys(), (QStringList{"displayName", "email", "firstName", "lastName", "username", "uuid"}));
        QCOMPARE(profile.value("uuid").toString(), userId);
        QCOMPARE(profile.value("displayName").toString(), QString("Cassandra Volkova"));
        QCOMPARE(profile.value("username").toString(), QString("synthetic-cassi"));
        QCOMPARE(profile.value("firstName").toString(), QString("Cassandra"));
        QCOMPARE(profile.value("lastName").toString(), QString("Volkova"));
        QCOMPARE(profile.value("email").toString(), QString("cassi@example.invalid"));
        const auto serialized = QJsonDocument(QJsonObject::fromVariantMap(profile)).toJson();
        QVERIFY(!serialized.contains("synthetic-private"));
        QVERIFY(!serialized.contains("arbitrary-avatar"));
        publicUser["first_name"] = "Updated";
        session.requestRefresh();
        QTRY_COMPARE(ready.size(), 2);
        QCOMPARE(session.publicProfile().value("displayName").toString(), QString("Updated Volkova"));
        session.logout();
        QVERIFY(session.publicProfile().isEmpty());
    }

    void profileIdentityMismatchNeverPublishesRejectedData()
    {
        MockServer server;
        bool mismatch = false;
        server.handler = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/introspect")) {
                response.data = QJsonObject{{"project_id", mismatch ? streamId : projectId},
                    {"user_info", QJsonObject{{"uuid", userId}, {"username", "synthetic-user"},
                        {"first_name", mismatch ? "Rejected" : "Verified"}}}};
            }
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy ready(&session, &Session::authenticated);
        QSignalSpy failures(&session, &Session::error);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        mismatch = true;
        session.selectProject(projectId);
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(session.publicProfile().isEmpty());
        QCOMPARE(ready.size(), 0);
        mismatch = false;
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(session.publicProfile().value("displayName").toString(), QString("Verified"));
        mismatch = true;
        session.requestRefresh();
        QTRY_COMPARE(failures.size(), 2);
        QCOMPARE(ready.size(), 1);
        QVERIFY(session.publicProfile().isEmpty());
    }

    void newAccountAndLogoutClearProfileBeforeAsyncRequestsComplete()
    {
        MockServer server;
        QString currentUser = userId;
        QString firstName = "First";
        server.handler = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/introspect"))
                response.data = QJsonObject{{"project_id", projectId},
                    {"user_info", QJsonObject{{"uuid", currentUser}, {"first_name", firstName}}}};
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        session.login(server.origin(), "first-login", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        QCOMPARE(session.publicProfile().value("displayName").toString(), QString("First"));
        currentUser = streamId;
        firstName = "Second";
        session.login(server.origin(), "second-login", "synthetic-password");
        QVERIFY(session.publicProfile().isEmpty());
        QTRY_COMPARE(session.projects().size(), 1);
        QVERIFY(session.publicProfile().isEmpty());
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        QCOMPARE(session.publicProfile().value("uuid").toString(), streamId);
        QCOMPARE(session.publicProfile().value("displayName").toString(), QString("Second"));
        QCOMPARE(session.publicProfile().value("username").toString(), QString("second-login"));
        session.logout();
        QVERIFY(session.publicProfile().isEmpty());
    }

    void malformedPublicFieldsUseLoginFallbackWithoutRawObjects()
    {
        MockServer server;
        server.handler = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", QJsonObject{
                    {"uuid", userId}, {"username", QJsonObject{{"token", "synthetic-private"}}},
                    {"first_name", QJsonArray{"synthetic-private"}}, {"last_name", 3}, {"email", true}}}};
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        session.login(server.origin(), "  fallback-login  ", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        const auto profile = session.publicProfile();
        QCOMPARE(profile.value("displayName").toString(), QString("fallback-login"));
        QCOMPARE(profile.value("username").toString(), QString("fallback-login"));
        QVERIFY(profile.value("firstName").toString().isEmpty());
        QVERIFY(profile.value("lastName").toString().isEmpty());
        QVERIFY(profile.value("email").toString().isEmpty());
    }

    void remembersOnlyVerifiedIdentityAndRotatedRefreshToken()
    {
        QTemporaryDir directory;
        SessionMemorySecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        HttpsSessionTransport transport;
        ApiClient api;
        Session session(&api, nullptr, transport.executor());
        session.setVault(&vault);
        session.login(QUrl("https://workspace.example"), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        QVERIFY(vault.accounts().isEmpty());
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        QTRY_COMPARE(vault.accounts().size(), 1);
        const auto saved = vault.accounts().first();
        QCOMPARE(saved.user, userId);
        QCOMPARE(saved.project, projectId);
        QCOMPARE(saved.login, QString("synthetic-user"));
        QCOMPARE(backend.values.value(saved.id), QByteArray("synthetic-rotated-refresh"));
        transport.rotated = "synthetic-second-rotation";
        session.requestRefresh();
        QTRY_COMPARE(backend.values.value(saved.id), QByteArray("synthetic-second-rotation"));
    }

    void resumesSavedAccountAfterRefreshAndIdentityVerification()
    {
        QTemporaryDir directory;
        SessionMemorySecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        const auto saved = savedIdentity();
        bool stored = false;
        vault.save(saved, "synthetic-saved-refresh", [&](QString error) { QVERIFY(error.isEmpty()); stored = true; });
        QTRY_VERIFY(stored);
        HttpsSessionTransport transport;
        ApiClient api;
        Session session(&api, nullptr, transport.executor());
        session.setVault(&vault);
        session.resumeSavedAccount(saved.id);
        QTRY_VERIFY(session.ready());
        QCOMPARE(api.server(), saved.server);
        QCOMPARE(session.user(), saved.user);
        QCOMPARE(session.project(), saved.project);
        QCOMPARE(transport.requests.first().body.value("grant_type").toString(), QString("refresh_token"));
        QCOMPARE(transport.requests.first().body.value("refresh_token").toString(), QString("synthetic-saved-refresh"));
        QCOMPARE(transport.requests.first().body.value("scope").toString(), "openid email profile project:" + projectId);
        QTRY_COMPARE(backend.values.value(saved.id), QByteArray("synthetic-rotated-refresh"));
        session.logout();
        QTRY_VERIFY(vault.accounts().isEmpty());
        QTRY_VERIFY(backend.values.isEmpty());
    }

    void rejectsResumedUserOrProjectMismatch()
    {
        for (const bool wrongUser : {false, true}) {
            QTemporaryDir directory;
            SessionMemorySecrets backend;
            SessionVault vault(directory.filePath("accounts.ini"), &backend);
            const auto saved = savedIdentity();
            bool stored = false;
            vault.save(saved, "synthetic-saved-refresh", [&](QString error) { QVERIFY(error.isEmpty()); stored = true; });
            QTRY_VERIFY(stored);
            HttpsSessionTransport transport;
            if (wrongUser) transport.identityUser = streamId;
            else transport.identityProject = streamId;
            ApiClient api;
            Session session(&api, nullptr, transport.executor());
            session.setVault(&vault);
            QSignalSpy failures(&session, &Session::error);
            QSignalSpy ready(&session, &Session::authenticated);
            session.resumeSavedAccount(saved.id);
            QTRY_VERIFY(!failures.isEmpty());
            QVERIFY(!session.ready());
            QVERIFY(session.publicProfile().isEmpty());
            QCOMPARE(ready.size(), 0);
            QTRY_VERIFY(vault.accounts().isEmpty());
        }
    }

    void offlineResumeAndInternalLoginResetKeepSavedAccounts()
    {
        QTemporaryDir directory;
        SessionMemorySecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        const auto saved = savedIdentity();
        bool stored = false;
        vault.save(saved, "synthetic-saved-refresh", [&](QString error) { QVERIFY(error.isEmpty()); stored = true; });
        QTRY_VERIFY(stored);
        HttpsSessionTransport transport;
        transport.offline = true;
        ApiClient api;
        Session session(&api, nullptr, transport.executor());
        session.setVault(&vault);
        QSignalSpy failures(&session, &Session::error);
        session.resumeSavedAccount(saved.id);
        QTRY_VERIFY(!failures.isEmpty());
        QVERIFY(!session.ready());
        QCOMPARE(vault.accounts().size(), 1);
        QCOMPARE(backend.values.value(saved.id), QByteArray("synthetic-saved-refresh"));
        session.login(QUrl("https://another.example"), "other-user", "synthetic-password");
        QTRY_COMPARE(failures.size(), 2);
        QCOMPARE(vault.accounts().size(), 1);
        transport.offline = false;
        session.resumeSavedAccount(saved.id);
        QTRY_VERIFY(session.ready());
        QCOMPARE(vault.accounts().size(), 1);
    }

    void terminalResumeRefreshInvalidatesOnlyItsSavedIdentity()
    {
        QTemporaryDir directory;
        SessionMemorySecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        const auto saved = savedIdentity();
        auto other = saved;
        other.server = QUrl("https://another.example");
        other.id = SessionVault::accountId(other.server, other.project, other.user);
        int stored = 0;
        vault.save(saved, "synthetic-saved-refresh", [&](QString error) { QVERIFY(error.isEmpty()); ++stored; });
        vault.save(other, "synthetic-other-refresh", [&](QString error) { QVERIFY(error.isEmpty()); ++stored; });
        QTRY_COMPARE(stored, 2);
        HttpsSessionTransport transport;
        transport.rejectRefresh = true;
        ApiClient api;
        Session session(&api, nullptr, transport.executor());
        session.setVault(&vault);
        QSignalSpy failures(&session, &Session::error);
        session.resumeSavedAccount(saved.id);
        QTRY_VERIFY(!failures.isEmpty());
        QTRY_COMPARE(vault.accounts().size(), 1);
        QCOMPARE(vault.accounts().first().id, other.id);
        QTRY_VERIFY(!backend.values.contains(saved.id));
        QVERIFY(backend.values.contains(other.id));
    }

    void authenticatesWithOtpAndSelectedProjectScope()
    {
        MockServer server;
        server.handler = sessionResponse;
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy projects(&session, &Session::projectsAvailable);
        QSignalSpy ready(&session, &Session::authenticated);
        session.login(server.origin(), "synthetic-user", "synthetic-password", "123456");
        QTRY_COMPARE(projects.size(), 1);
        QCOMPARE(session.projects().size(), 1);
        QVERIFY(!session.ready());
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(session.ready());
        QCOMPARE(session.user(), userId);
        QCOMPARE(session.project(), projectId);
        QCOMPARE(server.requests.at(0).headers.value("x-otp"), "123456");
        QVERIFY(!server.requests.at(0).headers.contains("authorization"));
        QCOMPARE(server.requests.at(0).body.value("scope").toString(), "openid email profile");
        QCOMPARE(server.requests.at(2).body.value("scope").toString(),
                 "openid email profile project:" + projectId);
        QCOMPARE(server.requests.at(3).headers.value("authorization"), "Bearer synthetic-project-token");
        session.logout();
        QVERIFY(!session.ready());
        QVERIFY(session.user().isEmpty());
        QVERIFY(session.projects().isEmpty());
    }

    void rejectsProjectIdentityMismatch()
    {
        MockServer server;
        server.handler = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto json = response.data.toObject();
                json.insert("project_id", userId);
                response.data = json;
            }
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy projects(&session, &Session::projectsAvailable);
        QSignalSpy ready(&session, &Session::authenticated);
        QSignalSpy errors(&session, &Session::error);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(projects.size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!session.ready());
        QCOMPARE(ready.size(), 0);
    }

    void logoutFencesPendingLogin()
    {
        MockServer server;
        server.handler = [](const Request &request) {
            auto response = sessionResponse(request);
            response.delay = 150;
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy projects(&session, &Session::projectsAvailable);
        QSignalSpy errors(&session, &Session::error);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(server.requests.size(), 1);
        session.logout();
        QTest::qWait(200);
        QCOMPARE(projects.size(), 0);
        QCOMPARE(errors.size(), 0);
        QVERIFY(!session.ready());
    }

    void transientRenewalPreservesVerifiedIdentityAndRetries()
    {
        MockServer server;
        int refreshRequests = 0;
        server.handler = [&refreshRequests](const Request &request) {
            auto response = sessionResponse(request);
            if (request.body.value("grant_type") == "refresh_token") {
                ++refreshRequests;
                if (refreshRequests == 2)
                    return Response{QJsonObject{}, 503, 50, {}};
                auto token = response.data.toObject();
                token.insert("expires_in", 60);
                if (refreshRequests >= 3)
                    token.insert("access_token", "synthetic-renewed-token");
                response.data = token;
            }
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy ready(&session, &Session::authenticated);
        QSignalSpy signedOut(&session, &Session::signedOut);
        QSignalSpy errors(&session, &Session::error);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        signedOut.clear();
        const auto generation = api.generation();
        session.requestRefresh();
        session.requestRefresh();
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(session.ready());
        QCOMPARE(session.user(), userId);
        QCOMPARE(session.project(), projectId);
        QCOMPARE(api.generation(), generation);
        QCOMPARE(signedOut.size(), 0);
        QTest::qWait(100);
        QCOMPARE(refreshRequests, 2);
        QTRY_COMPARE(ready.size(), 2);
        QCOMPARE(refreshRequests, 3);
        QVERIFY(session.ready());
        QVERIFY(api.generation() > generation);
        QCOMPARE(signedOut.size(), 0);
    }

    void terminalRenewalRejectionSignsOutWithoutRetry()
    {
        MockServer server;
        int refreshRequests = 0;
        server.handler = [&refreshRequests](const Request &request) {
            if (request.body.value("grant_type") == "refresh_token" && ++refreshRequests > 1)
                return Response{QJsonObject{{"error", "invalid_grant"}}, 401, 0, {}};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy ready(&session, &Session::authenticated);
        QSignalSpy signedOut(&session, &Session::signedOut);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        signedOut.clear();
        session.requestRefresh();
        QTRY_COMPARE(signedOut.size(), 1);
        QVERIFY(!session.ready());
        QVERIFY(session.user().isEmpty());
        QVERIFY(session.project().isEmpty());
        QTest::qWait(1100);
        QCOMPARE(refreshRequests, 2);
    }

    void transientRetriesStopAtActualAccessTokenExpiry()
    {
        MockServer server;
        int refreshRequests = 0;
        server.handler = [&refreshRequests](const Request &request) {
            auto response = sessionResponse(request);
            if (request.body.value("grant_type") == "refresh_token") {
                if (++refreshRequests > 1)
                    return Response{QJsonObject{}, 503, 0, {}};
                auto token = response.data.toObject();
                token.insert("expires_in", 1);
                response.data = token;
            }
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy ready(&session, &Session::authenticated);
        QSignalSpy signedOut(&session, &Session::signedOut);
        QSignalSpy errors(&session, &Session::error);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        signedOut.clear();
        session.requestRefresh();
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(session.ready());
        QTRY_COMPARE(signedOut.size(), 1);
        QVERIFY(!session.ready());
        QTest::qWait(200);
        QCOMPARE(refreshRequests, 2);
    }

    void logoutFencesDelayedRenewal()
    {
        MockServer server;
        int refreshRequests = 0;
        server.handler = [&refreshRequests](const Request &request) {
            auto response = sessionResponse(request);
            if (request.body.value("grant_type") == "refresh_token" && ++refreshRequests > 1)
                response.delay = 150;
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy ready(&session, &Session::authenticated);
        QSignalSpy errors(&session, &Session::error);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        session.requestRefresh();
        QTRY_COMPARE(refreshRequests, 2);
        session.logout();
        const auto generation = api.generation();
        QTest::qWait(200);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(api.generation(), generation);
        QVERIFY(!session.ready());
    }

    void expiryFencesHungRenewalBeforeItsResponseArrives()
    {
        MockServer server;
        int refreshRequests = 0;
        server.handler = [&refreshRequests](const Request &request) {
            auto response = sessionResponse(request);
            if (request.body.value("grant_type") == "refresh_token") {
                auto token = response.data.toObject();
                token.insert("expires_in", 1);
                response.data = token;
                if (++refreshRequests > 1)
                    response.delay = 2000;
            }
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        QSignalSpy ready(&session, &Session::authenticated);
        QSignalSpy signedOut(&session, &Session::signedOut);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(ready.size(), 1);
        signedOut.clear();
        session.requestRefresh();
        QTRY_COMPARE(refreshRequests, 2);
        QTRY_COMPARE_WITH_TIMEOUT(signedOut.size(), 1, 1600);
        QVERIFY(!session.ready());
        const auto generation = api.generation();
        QTest::qWait(1200);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(signedOut.size(), 1);
        QCOMPARE(api.generation(), generation);
        QCOMPARE(refreshRequests, 2);
    }
};

QTEST_GUILESS_MAIN(SessionTest)
#include "session-test.moc"
