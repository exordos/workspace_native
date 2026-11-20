#include "support/native-test-harness.h"
#include "support/controller-chat-creation-fixtures.h"
#include "support/controller-navigation-fixtures.h"

// Private additive test fixture only. Actual existing Users HTTP collection and
// canonical existing-direct GET flow; no controller/QML directory injection.
static const QString avatarReferenceA = "urn:image:aaaaaaaa-0000-4000-8000-000000000001";
static const QString avatarReferenceB = "urn:image:bbbbbbbb-0000-4000-8000-000000000002";

static QJsonObject avatarReferencePerson(const QString &id, const QString &username,
                                        const QJsonValue &avatar = QJsonValue(QJsonValue::Undefined))
{
    QJsonObject row{{"uuid", id}, {"project_id", projectId}, {"username", username},
        {"source", "iam"}, {"status", "offline"}, {"first_name", QJsonValue::Null},
        {"last_name", QJsonValue::Null}, {"email", username + "@example.invalid"},
        {"last_ping_at", "2026-01-02T10:00:00Z"}, {"created_at", "2026-01-02T10:00:00Z"},
        {"updated_at", "2026-01-02T10:00:00Z"}};
    if (!avatar.isUndefined()) row.insert("avatar", avatar);
    return row;
}

class AvatarReferenceFixture {
public:
    // Captured data outlives DirectChatFixture's server/controller callbacks.
    QJsonArray directory;
    DirectChatFixture direct;
    explicit AvatarReferenceFixture(const QJsonValue &avatar)
    {
        directory = {avatarReferencePerson(userId, "avatar-self", avatar),
            avatarReferencePerson(channelMemberA, "avatar-peer", avatar),
            avatarReferencePerson(channelMemberB, "other-peer")};
        direct.seed();
        direct.fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto profile = raw.value("user_info").toObject();
                profile["avatar"] = avatarReferenceB; raw["user_info"] = profile; response.data = raw;
            }
            return response;
        }; // Decoy IAM field cannot replace the actual Workspace Users source.
        direct.fixture.users = [this](const Request &) { return Response{directory}; };
        direct.fixture.messages = [](const Request &request) {
            const QUrlQuery query(request.target);
            if (query.queryItemValue("stream_uuid") != directId
                || query.queryItemValue("topic_uuid") != channelDefault) return Response{QJsonArray{}};
            auto own = message("Actual own image-reference witness", messageId, channelDefault);
            own["stream_uuid"] = directId;
            auto peer = message("Actual peer image-reference witness", returnedId, channelDefault);
            peer["stream_uuid"] = directId; peer["author_uuid"] = channelMemberA; peer["is_own"] = false;
            return Response{QJsonArray{own, peer}};
        };
    }
    bool start()
    {
        auto &c = direct.fixture.controller;
        if (!direct.fixture.start()
            || !QTest::qWaitFor([&] { return c.directChatDirectoryVerified(); }, 2000)) return false;
        c.startDirectChat(channelMemberA);
        if (!QTest::qWaitFor([&] { return c.directChatPhase() == "ready"
            && c.selectedStream() == directId; }, 2000)) return false;
        c.selectTopic(channelDefault);
        return QTest::qWaitFor([&] { return !c.busy() && c.messages().size() == 2; }, 2000);
    }
    QVariantMap sidebar() const
    {
        for (const auto &row : direct.fixture.controller.streams())
            if (row.toMap().value("id").toString() == directId) return row.toMap();
        return {};
    }
};

// Additive presentation fixture only: the existing real Users/direct/history HTTP paths.
static QJsonObject presencePerson(const QString &id, const QString &username, const QJsonValue &status,
    const QString &updated = "2026-01-02T10:00:00Z", const QJsonValue &text = QJsonValue::Null,
    const QJsonValue &emoji = QJsonValue::Null, const QJsonValue &avatar = QJsonValue(avatarReferenceA))
{
    auto row = avatarReferencePerson(id, username, avatar);
    if (status.isUndefined()) row.remove("status"); else row["status"] = status;
    row["updated_at"] = updated;
    row["status_text"] = text; row["status_emoji"] = emoji;
    return row;
}

class OwnStatusFixture {
public:
    QTemporaryDir storage;
    NativePreferences preferences{storage.filePath("own-status.ini")};
    QJsonObject own = avatarReferencePerson(userId, "cassi", avatarReferenceA);
    QJsonObject posted, returned;
    int posts = 0, usersGets = 0, responseStatus = 200;
    bool held = false, differentOwner = false;
    std::function<void()> release;
    Fixture fixture; // All captured transport/settings facts outlive Controller/server.
    OwnStatusFixture()
    {
        own["status"] = "active"; own["status_text"] = "Before"; own["status_emoji"] = "☕";
        fixture.users = [this](const Request &) { ++usersGets; return Response{QJsonArray{own}}; };
        fixture.authentication = [this](const Request &request) {
            auto response = sessionResponse(request);
            if (differentOwner && request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(), user = raw.value("user_info").toObject();
                user["uuid"] = returnedId; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) {
            if (request.target.path() != "/api/workspace/v1/users/" + userId + "/actions/presence/invoke")
                return fallback(request);
            ++posts; posted = request.body;
            auto row = returned.isEmpty() ? own : returned;
            if (returned.isEmpty()) {
                row["status"] = posted.value("status"); row["status_text"] = posted.value("text");
                row["status_emoji"] = posted.value("emoji"); row["updated_at"] = "2026-01-02T10:01:00Z";
                row["last_ping_at"] = "2026-01-02T10:01:00Z";
            }
            Response response{row, responseStatus};
            if (held) response.defer = [this](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        fixture.controller.usePreferences(&preferences);
    }
    bool start()
    {
        return storage.isValid() && fixture.start()
            && QTest::qWaitFor([this] { return fixture.api.findChildren<QNetworkReply *>().isEmpty(); }, 2000);
    }
    QPointer<QNetworkReply> pending() const
    {
        for (auto *reply : fixture.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/" + userId + "/actions/presence/invoke"
                && reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "POST") return reply;
        return {};
    }
    ~OwnStatusFixture() { if (release) release(); }
};

// Issue #134: serves the IAM profile endpoint, the messenger user projection
// re-read and the avatar upload/reset invokes on top of the base fixture.
class ProfileFixture {
public:
    Fixture fixture;
    QJsonObject iamResponse, directoryRow;
    int iamGets = 0, iamPuts = 0, avatarPosts = 0, resetPosts = 0, directoryGets = 0;
    int iamGetStatus = 200, iamPutStatus = 200, directoryStatus = 200;
    int avatarStatus = 200, resetStatus = 200;
    QByteArray multipartBody, multipartContentType;
    QVector<QJsonObject> putBodies;
    const QString avatarRef = QStringLiteral("urn:image:77777777-7777-4777-8777-777777777777");
    ProfileFixture()
    {
        fixture.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "self"}, {"first_name", "Cassi"},
                {"last_name", "Volkova"}, {"email", "cassi@example.invalid"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}},
            QJsonObject{{"uuid", channelMemberA}, {"username", "alice"}, {"first_name", "Alice"},
                {"last_name", "Adams"}, {"email", "alice@example.invalid"}, {"status", "idle"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}}}}; };
        iamResponse = {{"uuid", userId}, {"username", "self"}, {"first_name", "Cassi"},
            {"last_name", "Volkova"}, {"email", "cassi@example.invalid"},
            {"phone", QJsonValue::Null}, {"description", "Hello"}};
        directoryRow = iamResponse;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/core/v1/iam/users/" + userId) {
                if (request.method == "GET") { ++iamGets; return {iamResponse, iamGetStatus}; }
                if (request.method == "PUT") { ++iamPuts; putBodies.append(request.body); return {iamResponse, iamPutStatus}; }
            }
            if (path == "/api/workspace/v1/messenger/users/" + userId && request.method == "GET") {
                ++directoryGets;
                if (directoryStatus != 200) return {QJsonObject{}, directoryStatus};
                return {directoryRow};
            }
            if (path == "/api/workspace/v1/messenger/users/" + userId + "/actions/avatar_upload/invoke") {
                ++avatarPosts;
                multipartBody = request.rawBody;
                multipartContentType = request.headers.value("content-type");
                auto row = directoryRow;
                row["avatar"] = avatarRef;
                return {row, avatarStatus};
            }
            if (path == "/api/workspace/v1/messenger/users/" + userId + "/actions/avatar_reset/invoke") {
                ++resetPosts;
                auto row = directoryRow;
                row["avatar"] = QJsonValue::Null;
                return {row, resetStatus};
            }
            return fallback(request);
        };
    }
    bool start() { return fixture.start(); }
};

class ControllerProfilesTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void usersHttpNamesMatchSourceAndPreserveLegacyAbsentFields_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("expectedAuthor");
        QTest::addColumn<QString>("expectedDirectory");
        QTest::newRow("canonical-first-last") << QString("canonical-first-last")
            << QString("Alice Synthetic") << QString("Alice Synthetic");
        QTest::newRow("canonical-present-null") << QString("canonical-present-null")
            << QString("alice") << QString("alice");
        QTest::newRow("canonical-present-whitespace") << QString("canonical-present-whitespace")
            << QString("alice") << QString("alice");
        QTest::newRow("legacy-name-when-both-absent") << QString("legacy-name")
            << QString("Legacy name") << QString("alice");
        QTest::newRow("legacy-display-name-when-both-absent") << QString("legacy-display-name")
            << QString("Legacy display name") << QString("alice");
    }

    void usersHttpNamesMatchSourceAndPreserveLegacyAbsentFields()
    {
        QFETCH(QString, scenario); QFETCH(QString, expectedAuthor); QFETCH(QString, expectedDirectory);
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)}; auto &c = test.direct.fixture.controller;
        auto peer = test.directory[1].toObject();
        peer["username"] = "alice"; peer["email"] = "alice@example.invalid";
        peer["name"] = "Legacy name"; peer["display_name"] = "Legacy display name";
        if (scenario == "canonical-first-last") {
            peer["first_name"] = " Alice "; peer["last_name"] = " Synthetic ";
        } else if (scenario == "canonical-present-whitespace") {
            peer["first_name"] = " \t"; peer["last_name"] = "\n ";
        } else if (scenario.startsWith("legacy-")) {
            peer.remove("first_name"); peer.remove("last_name");
            if (scenario == "legacy-display-name") peer.remove("name");
        } // The canonical null row retains the actual nullable Users fields.
        test.directory[1] = peer; // Before the actual Users HTTP collection is served.
        QVERIFY(test.start());
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets, 1); QCOMPARE(test.direct.gets, 1); QCOMPARE(test.direct.posts, 0);
        QVERIFY(c.directChatDirectoryVerified()); QVERIFY(c.isDirectConversation());
        QCOMPARE(c.selectedStream(), directId); QCOMPARE(c.selectedTopic(), channelDefault);
        QCOMPARE(c.directPeer().value("displayName").toString(), expectedDirectory);
        QCOMPARE(test.sidebar().value("displayName").toString(), expectedDirectory);
        const auto displayed = modelMessage(c, returnedId);
        QCOMPARE(displayed.value("authorId").toString(), channelMemberA);
        QCOMPARE(displayed.value("content").toString(), QString("Actual peer image-reference witness"));
        QVERIFY(!displayed.value("own").toBool());
        QCOMPARE(displayed.value("authorAvatar").toString(), avatarReferenceA);
        QCOMPARE(c.directPeer().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(c.viewerDisplayName(), QString("avatar-self"));
        QCOMPARE(modelMessage(c, messageId).value("author").toString(), QString("avatar-self"));
        // First intended behavioral RED: current HTTP projection ignores canonical name parts.
        QCOMPARE(displayed.value("author").toString(), expectedAuthor);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/"));
    }

    void ownStatusContextTracksOnlyAuthenticatedOwnerAndTransportGeneration()
    {
        OwnStatusFixture test; auto &f = test.fixture; auto &c = f.controller;
        QVERIFY(c.metaObject()->indexOfProperty("ownStatusContextKey") >= 0);
        const auto context = [&] { return c.property("ownStatusContextKey").toString(); };
        QVERIFY(context().isEmpty());
        QVERIFY(test.start());
        QVERIFY(c.authenticated()); QVERIFY(c.ownerKey().isEmpty()); // Real loopback, no HTTPS Vault owner.
        const auto original = context();
        QCOMPARE(original.size(), 64);
        QCOMPARE(QByteArray::fromHex(original.toLatin1()).toHex(), original.toLatin1());
        const auto generation = f.api.generation();

        // Actual public route changes do not own this account-level editor.
        c.selectTopic({}); QCOMPARE(c.selectedTopic(), QString{}); QCOMPARE(context(), original);
        c.selectTopic(topicId); QTRY_VERIFY(!c.busy()); QCOMPARE(context(), original);
        c.refresh(); QTRY_COMPARE(test.usersGets, 2);
        QTRY_VERIFY(!c.busy() && f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.api.generation(), generation); QCOMPARE(context(), original);

        // Actual Users/presence admission publishes state without replacing the editor context.
        auto live = test.own;
        live["first_name"] = "Live"; live["email"] = "live@example.invalid";
        live["status"] = "do_not_disturb"; live["status_text"] = "Live metadata";
        live["updated_at"] = "2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", live, 286)));
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Live metadata"));
        QCOMPARE(c.account().value("displayEmail").toString(), QString("live@example.invalid"));
        QCOMPARE(context(), original); QCOMPARE(f.api.generation(), generation);

        QSignalSpy changed(&c, &NativeController::stateChanged); QVERIFY(changed.isValid());
        f.api.setAccessToken("synthetic-status-context-generation");
        QVERIFY(f.api.generation() > generation); QVERIFY(c.authenticated());
        QVERIFY(!context().isEmpty()); QVERIFY(context() != original); QVERIFY(changed.count() > 0);
        const auto replacement = context();
        const auto replacementGeneration = f.api.generation();
        f.api.setAccessToken("synthetic-status-context-generation");
        QCOMPARE(f.api.generation(), replacementGeneration); QCOMPARE(context(), replacement);
        c.logout(); QVERIFY(!c.authenticated()); QVERIFY(context().isEmpty());

        test.differentOwner = true; test.own["uuid"] = returnedId;
        QVERIFY(test.start()); QVERIFY(c.authenticated()); QCOMPARE(f.session.user(), returnedId);
        QVERIFY(!context().isEmpty()); QVERIFY(context() != original); QVERIFY(context() != replacement);
        QCOMPARE(test.posts, 0);
    }

    void ownStatusDraftDistinguishesMeasuredIdleFromDnd_data()
    {
        QTest::addColumn<QString>("status"); QTest::addColumn<bool>("away");
        QTest::newRow("measured-idle-is-not-deliberate-away") << QString("idle") << false;
        QTest::newRow("received-dnd-seeds-away") << QString("do_not_disturb") << true;
    }
    void ownStatusDraftDistinguishesMeasuredIdleFromDnd()
    {
        QFETCH(QString, status); QFETCH(bool, away);
        OwnStatusFixture test; test.own["status"] = status;
        test.own["status_text"] = " Focus "; test.own["status_emoji"] = "☕";
        QVERIFY(test.start()); auto &c = test.fixture.controller;
        QCOMPARE(test.usersGets, 1); QCOMPARE(c.account().value("presence").toString(), QString("idle"));
        const auto identity = test.fixture.session.publicProfile();
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Focus"));
        QCOMPARE(c.ownStatus().value("emoji").toString(), QString("☕"));
        QCOMPARE(c.ownStatus().value("away").toBool(), away);
        for (int i = 0; i < 20; ++i) QVERIFY(!c.ownStatus().value("pending").toBool());
        QCOMPARE(test.posts, 0); QCOMPARE(test.usersGets, 1);
        QCOMPARE(test.fixture.session.publicProfile(), identity);
    }

    void ownStatusPublishesOnlyTheCanonicalResponseAndRemembersTheChoice_data()
    {
        QTest::addColumn<bool>("away");
        QTest::newRow("save-deliberate-away") << true;
        QTest::newRow("save-cleared-active") << false;
    }
    void ownStatusPublishesOnlyTheCanonicalResponseAndRemembersTheChoice()
    {
        QFETCH(bool, away); OwnStatusFixture test; QVERIFY(test.start()); test.held = true;
        auto &f = test.fixture; auto &c = f.controller;
        const auto identity = f.session.publicProfile(), before = c.account();
        const auto beforeDraft = c.ownStatus();
        c.saveOwnStatus(away ? "  Focus \t" : " \n", away ? " ☕ " : " ", away);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(bool(test.release)); QTRY_VERIFY(test.pending());
        QCOMPARE(test.posted.value("status").toString(), away ? QString("idle") : QString("active"));
        if (away) {
            QCOMPARE(test.posted.value("text").toString(), QString("Focus"));
            QCOMPARE(test.posted.value("emoji").toString(), QString("☕"));
        } else { QVERIFY(test.posted.value("text").isNull()); QVERIFY(test.posted.value("emoji").isNull()); }
        QVERIFY(c.ownStatus().value("pending").toBool());
        QCOMPARE(c.ownStatus().value("text"), beforeDraft.value("text"));
        QCOMPARE(c.account(), before); // No optimistic display/presence, identity or avatar mutation.
        c.saveOwnStatus("Forbidden duplicate", "", !away); QCOMPARE(test.posts, 1);
        auto send = std::move(test.release); test.release = {}; send();
        QTRY_VERIFY(!c.ownStatus().value("pending").toBool());
        QVERIFY(c.ownStatus().value("error").toString().isEmpty());
        QCOMPARE(c.ownStatus().value("text").toString(), away ? QString("Focus") : QString{});
        QCOMPARE(c.ownStatus().value("away").toBool(), away);
        QCOMPARE(c.account().value("presence").toString(), away ? QString("idle") : QString("active"));
        QCOMPARE(c.account().value("email"), before.value("email"));
        QCOMPARE(c.account().value("avatar"), before.value("avatar"));
        QCOMPARE(f.session.publicProfile(), identity); QCOMPARE(test.posts, 1); QCOMPARE(test.usersGets, 1);
        // New preferences object + new Controller, same process/session: persisted manual witness,
        // not a claimed OS/process restart. No model or private setting is injected.
        NativePreferences reopened(test.storage.filePath("own-status.ini"));
        NativeController restored(&f.api, &f.session); restored.usePreferences(&reopened);
        QCOMPARE(restored.ownStatus().value("away").toBool(), away);
    }

    void ownStatusFailureCannotPublishOrRememberAnUnconfirmedChoice_data()
    {
        QTest::addColumn<bool>("wrongUser");
        QTest::newRow("transport-503") << false;
        QTest::newRow("successful-other-user-dto") << true;
    }
    void ownStatusFailureCannotPublishOrRememberAnUnconfirmedChoice()
    {
        QFETCH(bool, wrongUser); OwnStatusFixture test; QVERIFY(test.start());
        auto &c = test.fixture.controller;
        if (wrongUser) { test.returned = test.own; test.returned["uuid"] = returnedId; }
        else test.responseStatus = 503;
        const auto before = c.account();
        c.saveOwnStatus("Unconfirmed", "☕", true);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!c.ownStatus().value("pending").toBool());
        QVERIFY(!c.ownStatus().value("error").toString().isEmpty());
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Before"));
        QVERIFY(!c.ownStatus().value("away").toBool()); QCOMPARE(c.account(), before);
        NativePreferences reopened(test.storage.filePath("own-status.ini"));
        NativeController restored(&test.fixture.api, &test.fixture.session); restored.usePreferences(&reopened);
        QVERIFY(!restored.ownStatus().value("away").toBool()); QCOMPARE(test.posts, 1);
    }

    void ownStatusAcknowledgedReplyIsRetiredByCancelApiOrOwnerReplacement_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *s : {"cancel", "api-generation", "same-project-reauth", "logout-new-owner"})
            QTest::newRow(s) << QString::fromLatin1(s);
    }
    void ownStatusAcknowledgedReplyIsRetiredByCancelApiOrOwnerReplacement()
    {
        QFETCH(QString, change); OwnStatusFixture test; QVERIFY(test.start()); test.held = true;
        auto &f = test.fixture; auto &c = f.controller;
        c.saveOwnStatus("Retired", "☕", true);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(bool(test.release)); QTRY_VERIFY(test.pending());
        const auto pending = test.pending(); QVERIFY(pending && !pending->isFinished());
        if (change == "cancel") c.cancelOwnStatusUpdate();
        else if (change == "api-generation") f.api.setAccessToken("synthetic-own-status-replacement");
        else if (change == "same-project-reauth") f.session.selectProject(projectId);
        else {
            c.logout(); QVERIFY(c.account().isEmpty());
            test.differentOwner = true; test.own["uuid"] = returnedId;
            QVERIFY(f.start());
        }
        QTRY_VERIFY(pending.isNull() || pending->isFinished());
        QVERIFY(!c.ownStatus().value("pending").toBool());
        auto send = std::move(test.release); test.release = {}; send();
        QVERIFY(!c.ownStatus().value("pending").toBool());
        QVERIFY(c.ownStatus().value("text").toString() != "Retired");
        QVERIFY(!c.ownStatus().value("away").toBool()); QCOMPARE(test.posts, 1);
    }

    void ownStatusAdmissionCanBeCanceledReentrantlyBeforeAnyPost()
    {
        OwnStatusFixture test; QVERIFY(test.start()); auto &c = test.fixture.controller;
        bool armed = true; int interrupted = 0;
        const auto connection = connect(&c, &NativeController::stateChanged, this, [&] {
            if (armed && c.ownStatus().value("pending").toBool()) {
                armed = false; ++interrupted; c.cancelOwnStatusUpdate();
            }
        });
        auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        c.saveOwnStatus("Not admitted", "☕", true);
        QCOMPARE(interrupted, 1); QCOMPARE(test.posts, 0);
        QVERIFY(!c.ownStatus().value("pending").toBool());
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Before"));
        QVERIFY(!c.ownStatus().value("away").toBool());
    }

    void ownStatusHttpResponseKeepsNewerRawStatusAndExistingNameAvatarOrdering()
    {
        OwnStatusFixture test; QVERIFY(test.start()); test.held = true;
        auto &c = test.fixture.controller;
        c.saveOwnStatus("Older HTTP", "☕", false);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(bool(test.release)); QTRY_VERIFY(test.pending());
        auto live = test.own;
        live["first_name"] = "Live"; live["last_name"] = "Viewer"; live["avatar"] = avatarReferenceB;
        live["status"] = "do_not_disturb"; live["status_text"] = "Newer live"; live["status_emoji"] = "🌙";
        live["updated_at"] = "2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", live, 280)));
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Newer live"));
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceB);
        auto send = std::move(test.release); test.release = {}; send();
        QTRY_VERIFY(!c.ownStatus().value("pending").toBool());
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Newer live"));
        QCOMPARE(c.ownStatus().value("emoji").toString(), QString("🌙"));
        QVERIFY(c.ownStatus().value("away").toBool()); // Received DND, not inferred idle.
        QCOMPARE(c.account().value("presence").toString(), QString("idle"));
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceA); // Existing unversioned profile replacement stays independent.
        QCOMPARE(c.viewerDisplayName(), QString("cassi"));
        QCOMPARE(test.posts, 1); QCOMPARE(test.usersGets, 1);
    }

    void accountDisplayEmailUsesOwnedUsersBeforeIamWithoutChangingIdentity_data()
    {
        QTest::addColumn<QJsonValue>("usersEmail");
        QTest::addColumn<QString>("iamEmail");
        QTest::addColumn<QString>("expected");
        const auto canonical = QString(QChar(0xfeff)) + "  cassi@example.invalid \t" + QChar(0xfeff);
        QTest::newRow("owned-users-over-iam") << QJsonValue(canonical) << QString("iam@example.invalid")
            << QString("cassi@example.invalid");
        QTest::newRow("owned-users-with-empty-iam") << QJsonValue(canonical) << QString{}
            << QString("cassi@example.invalid");
        QTest::newRow("blank-users-falls-back-to-iam") << QJsonValue(" \t\n")
            << QString("iam@example.invalid") << QString("iam@example.invalid");
        QTest::newRow("null-users-falls-back-to-iam") << QJsonValue(QJsonValue::Null)
            << QString("iam@example.invalid") << QString("iam@example.invalid");
        QTest::newRow("missing-users-email-falls-back-to-iam") << QJsonValue(QJsonValue::Undefined)
            << QString("iam@example.invalid") << QString("iam@example.invalid");
    }

    void accountDisplayEmailUsesOwnedUsersBeforeIamWithoutChangingIdentity()
    {
        QFETCH(QJsonValue, usersEmail); QFETCH(QString, iamEmail); QFETCH(QString, expected);
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)};
        auto &f = test.direct.fixture; auto &c = f.controller;
        auto own = test.directory[0].toObject();
        if (usersEmail.isUndefined()) own.remove("email"); else own["email"] = usersEmail;
        test.directory[0] = own;
        const auto authentication = f.authentication;
        f.authentication = [authentication, iamEmail](const Request &request) {
            auto response = authentication(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(), user = raw.value("user_info").toObject();
                user["email"] = iamEmail; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        QVERIFY(test.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets, 1); QCOMPARE(test.direct.posts, 0);
        const auto identity = f.session.publicProfile();
        QCOMPARE(identity.value("email").toString(), iamEmail);
        QVERIFY(!identity.contains("displayEmail"));
        QCOMPARE(c.account().value("uuid").toString(), userId);
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceA);
        // Actual Users HTTP/owned profile is ready before the new display-only oracle.
        QCOMPARE(c.account().value("displayEmail").toString(), expected);
        QCOMPARE(c.account().value("email").toString(), iamEmail);
        QCOMPARE(f.session.publicProfile(), identity);
    }

    void gravatarAvatarReferencesSurviveThePresentationProjection()
    {
        // Reference workspace_ui@75bd21f workspace-avatar-urn.lib.ts: a
        // urn:gravatar value is a real avatar source, so the server's value
        // must reach every surface instead of being stripped at parse time.
        const QString gravatar = QStringLiteral("urn:gravatar:0123456789abcdef0123456789abcdef");
        AvatarReferenceFixture test{QJsonValue(gravatar)}; auto &c = test.direct.fixture.controller;
        QVERIFY(test.start());
        QCOMPARE(test.sidebar().value("avatar").toString(), gravatar);
        QCOMPARE(c.directPeer().value("avatar").toString(), gravatar);
        QCOMPARE(modelMessage(c, returnedId).value("authorAvatar").toString(), gravatar);
        QCOMPARE(c.account().value("avatar").toString(), gravatar);
    }

    void ownRealtimeEmailNotifiesAndForeignEmailNeverReplacesTheViewer()
    {
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)};
        auto &f = test.direct.fixture; auto &c = f.controller;
        QVERIFY(test.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        const auto identity = f.session.publicProfile(); const auto accounts = c.savedAccounts();
        const int usersBefore = test.direct.usersGets;
        const auto nameBefore = c.viewerDisplayName();
        const auto avatarBefore = c.account().value("avatar");
        auto own = test.directory[0].toObject(); own["email"] = " \tupdated@example.invalid\n";
        own["updated_at"] = "2026-01-02T10:01:00Z";
        QSignalSpy changed(&c, &NativeController::stateChanged); QVERIFY(changed.isValid());
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", own, 200)));
        QVERIFY(!changed.isEmpty());
        QCOMPARE(c.account().value("displayEmail").toString(), QString("updated@example.invalid"));
        auto foreign = test.directory[1].toObject(); foreign["email"] = "foreign@example.invalid";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", foreign, 201)));
        QCOMPARE(c.account().value("displayEmail").toString(), QString("updated@example.invalid"));
        own["email"] = QJsonValue::Null; own["updated_at"] = "2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", own, 202)));
        QCOMPARE(c.account().value("displayEmail").toString(), identity.value("email").toString());
        QCOMPARE(c.account().value("email"), identity.value("email"));
        QCOMPARE(f.session.publicProfile(), identity); QCOMPARE(c.savedAccounts(), accounts);
        QCOMPARE(c.viewerDisplayName(), nameBefore); QCOMPARE(c.account().value("avatar"), avatarBefore);
        QCOMPARE(test.direct.usersGets, usersBefore); QCOMPARE(test.direct.posts, 0);
    }

    void ownedCachedEmailRemainsDisplayMetadataUntilTheActualUsersReply()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        bool hold = false; std::function<void()> release;
        NativeCacheService initial(directory.path(), {}, true);
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)};
        auto &f = test.direct.fixture; auto &c = f.controller;
        auto own = test.directory[0].toObject(); own["email"] = " cached@example.invalid ";
        test.directory[0] = own;
        f.users = [&](const Request &) {
            Response response{test.directory};
            if (hold) response.defer = [&](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        c.useCache(&initial); QVERIFY(test.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        const auto identity = f.session.publicProfile();
        QVERIFY(flushCache(initial));
        const auto saved = cacheEntry(initial, initial.lease(), {NativeCache::Kind::Users, {}, {}, {}});
        QVERIFY(saved);
        bool ownedEmailPersisted = false;
        for (const auto &value : saved->rows) {
            const auto row = value.toObject();
            if (row.value("uuid").toString() == userId)
                ownedEmailPersisted = row.value("email").toString() == " cached@example.invalid ";
        }
        QVERIFY(ownedEmailPersisted);
        c.setRealtimeMode(true);
        c.useCache(nullptr); initial.shutdown(); QTRY_VERIFY(initial.stopped());
        // Reopen the actual SQLite profile entry, with a current Users reply held.
        own["email"] = QJsonValue::Null; test.directory[0] = own;
        hold = true; const int before = test.direct.usersGets;
        NativeCacheService reopened(directory.path(), {}, true);
        NativeController restored(&f.api, &f.session);
        auto releaseOnExit = qScopeGuard([&] { if (release) release(); });
        restored.useCache(&reopened);
        QTRY_COMPARE(test.direct.usersGets, before + 1); QTRY_VERIFY(bool(release));
        QTRY_COMPARE(restored.account().value("avatar").toString(), avatarReferenceA);
        QTRY_VERIFY(restored.cacheStale());
        QPointer<QNetworkReply> pending;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/") pending = reply;
        QVERIFY(pending && !pending->isFinished());
        QCOMPARE(restored.account().value("displayEmail").toString(), QString("cached@example.invalid"));
        QCOMPARE(restored.account().value("email"), identity.value("email"));
        auto send = std::move(release); release = {}; hold = false; send();
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_COMPARE(restored.account().value("displayEmail").toString(), identity.value("email").toString());
        QCOMPARE(f.session.publicProfile(), identity);
        QCOMPARE(test.direct.usersGets, before + 1); QCOMPARE(test.direct.posts, 0);
    }

    void oldOwnerEmailCannotReturnAfterLogoutNewOwnerOrALateUsersReply()
    {
        bool newOwner = false, holdNext = false; int usersGets = 0;
        std::function<void()> releaseOld;
        Fixture f;
        f.users = [&](const Request &) {
            ++usersGets;
            auto oldUser = avatarReferencePerson(userId, "old-viewer");
            oldUser["email"] = "old-users@example.invalid";
            Response response{QJsonArray{oldUser}}; // Deliberately no Users row for the new owner.
            if (holdNext) { holdNext = false; response.defer = [&](std::function<void()> send) { releaseOld = std::move(send); }; }
            return response;
        };
        f.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(), user = raw.value("user_info").toObject();
                user["uuid"] = newOwner ? returnedId : userId;
                user["email"] = newOwner ? "new-iam@example.invalid" : "old-iam@example.invalid";
                raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        auto releaseOnExit = qScopeGuard([&] { if (releaseOld) releaseOld(); });
        QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("old-users@example.invalid"));
        holdNext = true; f.controller.refresh(); QTRY_COMPARE(usersGets, 2); QTRY_VERIFY(bool(releaseOld));
        QPointer<QNetworkReply> pending;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/") pending = reply;
        QVERIFY(pending && !pending->isFinished());
        const auto generation = f.api.generation(); f.controller.logout();
        QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.session.publicProfile().isEmpty());
        QTRY_VERIFY(pending.isNull() || pending->isFinished());
        newOwner = true; QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(f.api.generation() > generation); QCOMPARE(usersGets, 3);
        QCOMPARE(f.controller.account().value("uuid").toString(), returnedId);
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("new-iam@example.invalid"));
        QCOMPARE(f.controller.account().value("email").toString(), QString("new-iam@example.invalid"));
        auto send = std::move(releaseOld); releaseOld = {}; send();
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("new-iam@example.invalid"));
        auto oldEvent = controllerEvent("user.updated", avatarReferencePerson(userId, "old-viewer"), 240);
        QVERIFY(!f.controller.applyRealtimeEvent(oldEvent));
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("new-iam@example.invalid"));
        f.controller.logout(); QVERIFY(f.controller.account().isEmpty());
    }

    void presenceUsesActualUsersAndPreservesIndependentPresentation_data()
    {
        QTest::addColumn<QJsonValue>("wireStatus"); QTest::addColumn<QString>("expected");
        QTest::newRow("active") << QJsonValue("active") << QString("active");
        QTest::newRow("idle") << QJsonValue("idle") << QString("idle");
        QTest::newRow("do-not-disturb") << QJsonValue("do_not_disturb") << QString("idle");
        QTest::newRow("offline") << QJsonValue("offline") << QString("offline");
        QTest::newRow("absent") << QJsonValue(QJsonValue::Undefined) << QString{};
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << QString{};
        QTest::newRow("unknown-string") << QJsonValue("online") << QString{};
        QTest::newRow("invalid-object") << QJsonValue(QJsonObject{{"active",true}}) << QString{};
    }
    void presenceUsesActualUsersAndPreservesIndependentPresentation()
    {
        QFETCH(QJsonValue, wireStatus); QFETCH(QString, expected);
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        test.directory = {presencePerson(userId,"avatar-self",wireStatus),
            presencePerson(channelMemberA,"avatar-peer",wireStatus),
            avatarReferencePerson(channelMemberB,"other-peer")};
        QVERIFY2(test.start(),"Actual Users/direct/topic history must settle before presence assertions");
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets,1); QCOMPARE(test.direct.gets,1); QCOMPARE(test.direct.posts,0);
        QCOMPARE(c.account().value("uuid").toString(),userId);
        QCOMPARE(c.viewerDisplayName(),QString("avatar-self"));
        QCOMPARE(c.directPeer().value("displayName").toString(),QString("avatar-peer"));
        QCOMPARE(modelMessage(c,returnedId).value("author").toString(),QString("avatar-peer"));
        QCOMPARE(c.account().value("avatar").toString(),avatarReferenceA);
        QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceA);
        QCOMPARE(c.selectedStream(),directId); QCOMPARE(c.selectedTopic(),channelDefault);
        QVERIFY(c.directChatDirectoryVerified());
        QCOMPARE(c.channelMemberChoices().first().toMap(),QVariantMap({{"id",channelMemberA},
            {"displayName","avatar-peer"},{"username","avatar-peer"},{"email","avatar-peer@example.invalid"}}));
        const auto sidebar = test.sidebar(), peer = modelMessage(c,returnedId), own = modelMessage(c,messageId);
        QVERIFY(sidebar.contains("id") && peer.contains("id") && own.contains("id"));
        QVERIFY2(c.account().contains("presence"),"Actual Workspace Users presence is absent from account projection");
        QVERIFY(c.directPeer().contains("presence")); QVERIFY(sidebar.contains("presence"));
        QVERIFY(peer.contains("authorPresence")); QVERIFY(own.contains("authorPresence"));
        QCOMPARE(c.account().value("presence").toString(),expected);
        QCOMPARE(c.directPeer().value("presence").toString(),expected);
        QCOMPARE(sidebar.value("presence").toString(),expected);
        QCOMPARE(peer.value("authorPresence").toString(),expected);
        QCOMPARE(own.value("authorPresence").toString(),expected);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/")
                && !(request.method!="GET" && request.target.path().startsWith("/api/workspace/")));
    }

    void ownStatusLabelUsesSourceTrimAndEmojiRules_data()
    {
        QTest::addColumn<QJsonValue>("text"); QTest::addColumn<QJsonValue>("emoji");
        QTest::addColumn<QString>("expected");
        QTest::newRow("empty") << QJsonValue(" \t") << QJsonValue(" ") << QString{};
        QTest::newRow("unicode") << QJsonValue(" Focus ") << QJsonValue(QString::fromUtf8("☕")) << QString::fromUtf8("☕ Focus");
        const auto feff = QString(QChar(0xfeff));
        QTest::newRow("source-feff-trim") << QJsonValue(feff+"Focus"+feff) << QJsonValue(feff+"house"+feff) << QString::fromUtf8("🏠 Focus");
        QTest::newRow("unknown-shortcode") << QJsonValue(" Ship ") << QJsonValue("party_parrot") << QString("Ship");
        QTest::newRow("legacy-emoji-only") << QJsonValue(QJsonValue::Null) << QJsonValue("speech_balloon") << QString::fromUtf8("💬");
        QTest::newRow("kelvin-not-ascii-shortcode") << QJsonValue(QJsonValue::Null) << QJsonValue(QString::fromUtf8("K")) << QString::fromUtf8("K");
        QTest::newRow("long-s-not-ascii-shortcode") << QJsonValue(QJsonValue::Null) << QJsonValue(QString::fromUtf8("ſ")) << QString::fromUtf8("ſ");
    }
    void ownStatusLabelUsesSourceTrimAndEmojiRules()
    {
        QFETCH(QJsonValue,text); QFETCH(QJsonValue,emoji); QFETCH(QString,expected);
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        test.directory.replace(0,presencePerson(userId,"avatar-self","active","2026-01-02T10:00:00Z",text,emoji));
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.viewerDisplayName(),QString("avatar-self")); QCOMPARE(c.account().value("avatar").toString(),avatarReferenceA);
        QVERIFY2(c.account().contains("statusLabel"),"Source own custom-status projection is absent");
        QCOMPARE(c.account().value("statusLabel").toString(),expected);
        QCOMPARE(c.account().value("presence").toString(),QString("active"));
        QCOMPARE(test.direct.usersGets,1); QCOMPARE(test.direct.posts,0);
    }

    void directSidebarObservesCustomStatusOnlyChanges_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("emoji");
        QTest::addColumn<QString>("expectedEmoji");
        QTest::addColumn<bool>("changed");
        QTest::newRow("text-only") << QString("On call") << QString("house") << QString("🏠") << true;
        QTest::newRow("emoji-only") << QString("Focus") << QString("palm_tree") << QString("🌴") << true;
        QTest::newRow("clear") << QString() << QString() << QString() << true;
        QTest::newRow("same-display") << QString(" Focus ") << QString("🏠") << QString("🏠") << false;
    }

    void directSidebarObservesCustomStatusOnlyChanges()
    {
        QFETCH(QString, text);
        QFETCH(QString, emoji);
        QFETCH(QString, expectedEmoji);
        QFETCH(bool, changed);
        AvatarReferenceFixture test(avatarReferenceA);
        auto peer = presencePerson(channelMemberA, "avatar-peer", "active",
            "2026-01-02T10:00:00Z", "Focus", "house");
        test.directory[1] = peer;
        QVERIFY(test.start());
        auto &controller = test.direct.fixture.controller;
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        auto shown = test.sidebar();
        QCOMPARE(shown.value("statusText").toString(), QString("Focus"));
        QSignalSpy updates(&controller, &NativeController::sidebarStreamsChanged);
        connect(&controller, &NativeController::sidebarStreamsChanged, this, [&] { shown = test.sidebar(); });
        peer["status_text"] = text;
        peer["status_emoji"] = emoji;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("user.updated", peer, 83)));
        QCOMPARE(!updates.isEmpty(), changed);
        QCOMPARE(shown.value("statusText").toString(), text.trimmed());
        QCOMPARE(shown.value("statusEmoji").toString(), expectedEmoji);
    }

    void presenceOnlyRealtimeNotifiesExistingSurfacesWithoutChangingIdentity()
    {
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto choices = c.channelMemberChoices(); const auto context = c.directChatContextKey();
        const auto account = c.account();
        QSignalSpy state(&c,&NativeController::stateChanged), streams(&c,&NativeController::streamsChanged),
            messages(&c,&NativeController::messagesChanged);
        QVERIFY(state.isValid() && streams.isValid() && messages.isValid());
        const QStringList statuses{"active","idle","do_not_disturb","offline"};
        const QStringList visuals{"active","idle","idle","offline"};
        QString previous = "offline"; int epoch = 80;
        for (qsizetype i=0;i<statuses.size();++i) {
            state.clear(); streams.clear(); messages.clear();
            const auto stamp = QString("2026-01-02T10:0%1:00Z").arg(i+1);
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(channelMemberA,"avatar-peer",statuses[i],stamp),epoch++)));
            QCOMPARE(c.directPeer().value("displayName").toString(),QString("avatar-peer"));
            QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceA);
            QCOMPARE(c.directPeer().value("presence").toString(),visuals[i]);
            QCOMPARE(test.sidebar().value("presence").toString(),visuals[i]);
            QCOMPARE(modelMessage(c,returnedId).value("authorPresence").toString(),visuals[i]);
            if (previous!=visuals[i]) QVERIFY2(!streams.isEmpty(),"A pure presence transition must notify the real Direct sidebar model");
            QVERIFY(!messages.isEmpty()); QVERIFY(!state.isEmpty()); previous=visuals[i];
            state.clear();
            const auto label = QString("Own public status %1").arg(i);
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(userId,"avatar-self",statuses[i],stamp,label),epoch++)));
            QCOMPARE(c.account().value("presence").toString(),visuals[i]);
            QCOMPARE(c.account().value("statusLabel").toString(),label); QVERIFY(!state.isEmpty());
            QCOMPARE(c.account().value("displayName"),account.value("displayName"));
            QCOMPARE(c.account().value("avatar").toString(),avatarReferenceA);
            QCOMPARE(c.channelMemberChoices(),choices); QCOMPARE(c.directChatContextKey(),context);
            QCOMPARE(c.selectedStream(),directId); QCOMPARE(c.selectedTopic(),channelDefault);
        }
        QCOMPARE(test.direct.usersGets,1); QCOMPARE(test.direct.posts,0);
        for (const auto &request : test.direct.fixture.server.requests) QVERIFY(!request.target.path().contains("/files/"));
    }

    void olderPresenceKeepsNewFactsWithoutChangingLegacyReplacementOrder_data()
    {
        QTest::addColumn<bool>("throughUsers"); QTest::addColumn<QJsonValue>("wireUpdatedAt");
        QTest::newRow("admitted-realtime") << false << QJsonValue("2026-01-02T10:04:00Z");
        QTest::newRow("deferred-users-http") << true << QJsonValue("2026-01-02T10:04:00Z");
        QTest::newRow("missing-time-admitted-realtime") << false << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("invalid-time-admitted-realtime") << false << QJsonValue("not-a-timestamp");
    }
    void olderPresenceKeepsNewFactsWithoutChangingLegacyReplacementOrder()
    {
        QFETCH(bool,throughUsers); QFETCH(QJsonValue,wireUpdatedAt);
        bool hold=false; std::function<void()> release;
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        test.directory = {presencePerson(userId,"avatar-self","active","2026-01-02T10:05:00Z","Current"),
            presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:05:00Z"),
            avatarReferencePerson(channelMemberB,"other-peer")};
        test.direct.fixture.users = [&](const Request &) {
            Response response{test.directory};
            if(hold) { hold=false; response.defer=[&](std::function<void()> send){release=std::move(send);}; }
            return response;
        };
        auto releaseOnExit=qScopeGuard([&] { if(release) release(); });
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        auto oldSelf=presencePerson(userId,"older-self","idle","2026-01-02T10:04:00Z","Older",QJsonValue::Null,avatarReferenceB);
        auto oldPeer=presencePerson(channelMemberA,"older-peer","idle","2026-01-02T10:04:00Z",QJsonValue::Null,QJsonValue::Null,avatarReferenceB);
        if(wireUpdatedAt.isUndefined()) { oldSelf.remove("updated_at"); oldPeer.remove("updated_at"); }
        else { oldSelf["updated_at"]=wireUpdatedAt; oldPeer["updated_at"]=wireUpdatedAt; }
        if(throughUsers) {
            test.directory = {oldSelf,oldPeer,avatarReferencePerson(channelMemberB,"other-peer")};
            hold=true; c.refresh(); QTRY_COMPARE(test.direct.usersGets,2); QTRY_VERIFY(bool(release));
            QPointer<QNetworkReply> pending;
            for(auto *reply:test.direct.fixture.api.findChildren<QNetworkReply *>())
                if(!reply->isFinished()&&reply->url().path()=="/api/workspace/v1/users/") pending=reply;
            QVERIFY(pending && !pending->isFinished());
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:06:00Z"),100)));
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(userId,"avatar-self","active","2026-01-02T10:06:00Z","Current"),101)));
            auto send=std::move(release); release={}; send();
            QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        } else {
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",oldPeer,100)));
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",oldSelf,101)));
        }
        // The old name/avatar replacement remains observable; only new status facts are ordered.
        QCOMPARE(c.directPeer().value("displayName").toString(),QString("older-peer"));
        QCOMPARE(c.viewerDisplayName(),QString("older-self"));
        QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceB);
        QCOMPARE(c.account().value("avatar").toString(),avatarReferenceB);
        QCOMPARE(c.account().value("presence").toString(),QString("active"));
        QCOMPARE(c.account().value("statusLabel").toString(),QString("Current"));
        QCOMPARE(c.directPeer().value("presence").toString(),QString("active"));
        QCOMPARE(test.sidebar().value("presence").toString(),QString("active"));
        QCOMPARE(modelMessage(c,returnedId).value("authorPresence").toString(),QString("active"));
        QCOMPARE(test.direct.posts,0);
    }

    // Issue #88 (reference 61dacef7 chat-header.types.ts:1-17 and
    // chat-header.lib.ts:20-27): the direct header caption consumes
    // customStatus (custom status with its emoji, shown as-is) and the
    // last-seen display string. The authoritative users directory rows are
    // the native projection source, with the documented caveat that the
    // reference production selector assigns neither fact at the pinned sha
    // (docs/NATIVE_DESKTOP_DM_HEADER_STATUS.md).
    void directPeerProjectsCustomStatusAndLastSeenFromDirectoryRows_data()
    {
        QTest::addColumn<QJsonValue>("statusText");
        QTest::addColumn<QJsonValue>("statusEmoji");
        QTest::addColumn<QJsonValue>("lastSeen");
        QTest::addColumn<QString>("customStatus");
        QTest::addColumn<QString>("lastSeenText");
        QTest::newRow("text-only") << QJsonValue("On call") << QJsonValue(QJsonValue::Null)
            << QJsonValue("15:30") << QString("On call") << QString("15:30");
        QTest::newRow("emoji-only") << QJsonValue(QJsonValue::Null)
            << QJsonValue(QString::fromUtf8("\xF0\x9F\x93\x9E")) << QJsonValue(QJsonValue::Undefined)
            << QString::fromUtf8("\xF0\x9F\x93\x9E") << QString();
        QTest::newRow("emoji-and-text") << QJsonValue("On call")
            << QJsonValue(QString::fromUtf8("\xF0\x9F\x93\x9E")) << QJsonValue("15:30")
            << QString::fromUtf8("\xF0\x9F\x93\x9E On call") << QString("15:30");
        QTest::newRow("both-absent") << QJsonValue(QJsonValue::Null) << QJsonValue(QJsonValue::Null)
            << QJsonValue(QJsonValue::Undefined) << QString() << QString();
        QTest::newRow("last-seen-null") << QJsonValue(QJsonValue::Null) << QJsonValue(QJsonValue::Null)
            << QJsonValue(QJsonValue::Null) << QString() << QString();
    }
    void directPeerProjectsCustomStatusAndLastSeenFromDirectoryRows()
    {
        QFETCH(QJsonValue, statusText); QFETCH(QJsonValue, statusEmoji); QFETCH(QJsonValue, lastSeen);
        QFETCH(QString, customStatus); QFETCH(QString, lastSeenText);
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        auto self = presencePerson(userId, "avatar-self", "active");
        auto peer = presencePerson(channelMemberA, "avatar-peer", "active",
            "2026-01-02T10:00:00Z", statusText, statusEmoji);
        if (!lastSeen.isUndefined()) peer["last_seen"] = lastSeen;
        test.directory = {self, peer, avatarReferencePerson(channelMemberB, "other-peer")};
        QVERIFY(test.start());
        QVERIFY2(c.directPeer().contains("customStatus"),
            "Direct peer custom status is absent from the header projection");
        QVERIFY2(c.directPeer().contains("lastSeen"),
            "Direct peer last-seen display string is absent from the header projection");
        QCOMPARE(c.directPeer().value("customStatus").toString(), customStatus);
        QCOMPARE(c.directPeer().value("lastSeen").toString(), lastSeenText);
        // The typing fact has no wire source among the handled realtime
        // kinds; the projection never invents it.
        QVERIFY(!c.directPeer().value("typing").toBool());
    }

    void directPeerStatusFactsFollowRealtimeUserUpdatedWithoutRegression()
    {
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        QVERIFY(test.start());
        QVERIFY(c.directPeer().value("customStatus").toString().isEmpty());
        QVERIFY(c.directPeer().value("lastSeen").toString().isEmpty());
        auto fresh = presencePerson(channelMemberA, "avatar-peer", "active",
            "2026-01-02T10:06:00Z", "On call");
        fresh["last_seen"] = "15:30";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", fresh, 250)));
        QCOMPARE(c.directPeer().value("customStatus").toString(), QString("On call"));
        QCOMPARE(c.directPeer().value("lastSeen").toString(), QString("15:30"));
        // An older row keeps the newest facts, including the new last seen.
        auto older = presencePerson(channelMemberA, "avatar-peer", "idle",
            "2026-01-02T10:05:30Z", "Older");
        older["last_seen"] = "09:00";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", older, 251)));
        QCOMPARE(c.directPeer().value("customStatus").toString(), QString("On call"));
        QCOMPARE(c.directPeer().value("lastSeen").toString(), QString("15:30"));
        // The latest row removed both optional facts: the caption inputs fall
        // back to empty (contract null), never the stale values.
        auto cleared = presencePerson(channelMemberA, "avatar-peer", "active",
            "2026-01-02T10:07:00Z");
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", cleared, 252)));
        QVERIFY(c.directPeer().value("customStatus").toString().isEmpty());
        QVERIFY(c.directPeer().value("lastSeen").toString().isEmpty());
    }

    void reopenedUsersProfilesAreOfflineUntilTheActualUsersReply()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        bool hold=false; std::function<void()> release;
        NativeCacheService initial(directory.path(),{},true);
        AvatarReferenceFixture test(avatarReferenceA); auto &c=test.direct.fixture.controller;
        test.directory = {presencePerson(userId,"avatar-self","active","2026-01-02T10:05:00Z","Current"),
            presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:05:00Z"),
            avatarReferencePerson(channelMemberB,"other-peer")};
        test.direct.fixture.users=[&](const Request &) {
            Response response{test.directory};
            if(hold) response.defer=[&](std::function<void()> send){release=std::move(send);};
            return response;
        };
        c.useCache(&initial); QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(flushCache(initial)); auto saved=cacheEntry(initial,initial.lease(),{NativeCache::Kind::Users,{},{},{}});
        QVERIFY(saved); QCOMPARE(saved->rows.size(),3);
        for(const auto &value:saved->rows) {
            const auto row=value.toObject(); QVERIFY(row.contains("username"));
            QVERIFY(!row.contains("status")); QVERIFY(!row.contains("status_text"));
            QVERIFY(!row.contains("status_emoji")); QVERIFY(!row.contains("last_ping_at"));
        }
        c.setRealtimeMode(true); // Existing public mode stops its poll; no fake event or store injection.
        QSignalSpy shutdownNotice(&initial,&NativeCacheService::shutdownFinished); QVERIFY(shutdownNotice.isValid());
        c.useCache(nullptr); initial.shutdown(); QTRY_VERIFY(initial.stopped());
        QCOMPARE(shutdownNotice.size(),1); // Real worker SQL close acknowledged before reopening.
        hold=true; const int before=test.direct.usersGets;
        NativeCacheService reopened(directory.path(),{},true);
        NativeController restored(&test.direct.fixture.api,&test.direct.fixture.session);
        auto releaseOnExit=qScopeGuard([&] { if(release) release(); });
        restored.useCache(&reopened);
        QTRY_COMPARE(test.direct.usersGets,before+1); QTRY_VERIFY(bool(release));
        QTRY_COMPARE(restored.account().value("avatar").toString(),avatarReferenceA);
        QTRY_VERIFY(restored.cacheStale());
        QCOMPARE(restored.account().value("presence").toString(),QString("offline"));
        QVERIFY(restored.account().contains("statusLabel"));
        QVERIFY(restored.account().value("statusLabel").toString().isEmpty());
        QPointer<QNetworkReply> pending;
        for(auto *reply:test.direct.fixture.api.findChildren<QNetworkReply *>())
            if(!reply->isFinished()&&reply->url().path()=="/api/workspace/v1/users/") pending=reply;
        QVERIFY(pending && !pending->isFinished());
        auto send=std::move(release); release={}; hold=false; send();
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_COMPARE(restored.account().value("presence").toString(),QString("active"));
        QCOMPARE(restored.account().value("statusLabel").toString(),QString("Current"));
        QCOMPARE(test.direct.usersGets,before+1); QCOMPARE(test.direct.posts,0);
    }

    void oldOwnerUsersCannotRestorePresenceAfterReplacementOrReset()
    {
        bool newOwner=false,holdNext=false; int usersGets=0; std::function<void()> releaseOld;
        Fixture f;
        f.users=[&](const Request &) {
            ++usersGets;
            Response response{QJsonArray{presencePerson(newOwner?returnedId:userId,newOwner?"new-viewer":"old-viewer",newOwner?"idle":"active"),
                presencePerson(channelMemberA,newOwner?"new-peer":"old-peer",newOwner?"idle":"active")}};
            if(holdNext) { holdNext=false; response.defer=[&](std::function<void()> send){releaseOld=std::move(send);}; }
            return response;
        };
        f.authentication=[&](const Request &request) {
            auto response=sessionResponse(request);
            if(newOwner&&request.target.path().endsWith("/actions/introspect")) {
                auto raw=response.data.toObject(), user=raw.value("user_info").toObject();
                user["uuid"]=returnedId; raw["user_info"]=user; response.data=raw;
            }
            return response;
        };
        f.messages=[](const Request &) {
            auto row=message("Actual owner-scoped presence history"); row["author_uuid"]=channelMemberA; row["is_own"]=false;
            return Response{QJsonArray{row}};
        };
        auto releaseOnExit=qScopeGuard([&] { if(releaseOld) releaseOld(); });
        QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.viewerDisplayName(),QString("old-viewer")); QCOMPARE(usersGets,1);
        holdNext=true; f.controller.refresh(); QTRY_COMPARE(usersGets,2); QTRY_VERIFY(bool(releaseOld));
        QPointer<QNetworkReply> oldReply;
        for(auto *reply:f.api.findChildren<QNetworkReply *>())
            if(!reply->isFinished()&&reply->url().path()=="/api/workspace/v1/users/") oldReply=reply;
        QVERIFY(oldReply && !oldReply->isFinished());
        const auto generation=f.api.generation(); f.controller.logout();
        QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.controller.messages().isEmpty()); QVERIFY(f.controller.streams().isEmpty());
        QTRY_VERIFY(oldReply.isNull()||oldReply->isFinished());
        newOwner=true; QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.account().value("uuid").toString(),returnedId); QCOMPARE(usersGets,3);
        QCOMPARE(f.controller.viewerDisplayName(),QString("new-viewer")); QVERIFY(f.api.generation()>generation);
        QCOMPARE(f.controller.account().value("presence").toString(),QString("idle"));
        QCOMPARE(modelMessage(f.controller,messageId).value("authorPresence").toString(),QString("idle"));
        auto send=std::move(releaseOld); releaseOld={}; send();
        QCOMPARE(f.controller.account().value("presence").toString(),QString("idle"));
        auto oldEvent=controllerEvent("user.updated",presencePerson(channelMemberA,"old-peer","active"),180);
        QVERIFY(!f.controller.applyRealtimeEvent(oldEvent));
        QCOMPARE(modelMessage(f.controller,messageId).value("authorPresence").toString(),QString("idle"));
        f.controller.logout(); QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.controller.messages().isEmpty());
    }

    void presenceOnlyUsersReplyNotifiesAfterTheOtherCatalogRepliesHaveSettled()
    {
        bool hold=false,observedCurrent=false; std::function<void()> release;
        AvatarReferenceFixture test(avatarReferenceA); auto &c=test.direct.fixture.controller;
        test.direct.fixture.users=[&](const Request &) {
            Response response{test.directory};
            if(hold) { hold=false; response.defer=[&](std::function<void()> send){release=std::move(send);}; }
            return response;
        };
        auto releaseOnExit=qScopeGuard([&] { if(release) release(); });
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto choices=c.channelMemberChoices(); const auto names=c.directPeer().value("displayName");
        test.directory.replace(1,presencePerson(channelMemberA,"avatar-peer","idle","2026-01-02T10:01:00Z"));
        hold=true; c.refresh(); QTRY_COMPARE(test.direct.usersGets,2); QTRY_VERIFY(bool(release));
        // Only the acknowledged, deliberately held Users request remains; earlier
        // Streams signals cannot satisfy the post-Users-publication witness.
        QTRY_COMPARE(test.direct.fixture.api.findChildren<QNetworkReply *>().size(),1);
        QPointer<QNetworkReply> pending=test.direct.fixture.api.findChildren<QNetworkReply *>().first();
        QVERIFY(pending&&!pending->isFinished()); QCOMPARE(pending->url().path(),QString("/api/workspace/v1/users/"));
        const auto observer=connect(&c,&NativeController::streamsChanged,&c,[&] {
            if(test.sidebar().value("presence").toString()=="idle") observedCurrent=true;
        });
        auto disconnectObserver=qScopeGuard([&] { disconnect(observer); });
        auto send=std::move(release); release={}; send();
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_VERIFY(!c.busy());
        QCOMPARE(c.directPeer().value("displayName"),names);
        QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceA);
        QCOMPARE(c.directPeer().value("presence").toString(),QString("idle"));
        QVERIFY2(observedCurrent,"The actual Users-only presence publication must notify the Direct sidebar");
        QCOMPARE(c.channelMemberChoices(),choices); QCOMPARE(test.direct.posts,0);
    }

    void aPresenceOnlySubscriberCanSignOutBeforeAnyOldOwnerRefill()
    {
        bool armed=false; int observations=0;
        AvatarReferenceFixture test(avatarReferenceA); auto &c=test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto observer=connect(&c,&NativeController::streamsChanged,&c,[&] {
            if(!armed) return;
            armed=false; ++observations; c.logout();
        });
        auto disconnectObserver=qScopeGuard([&] { disconnect(observer); });
        armed=true;
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
            presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:01:00Z"),190)));
        QCOMPARE(observations,1); QVERIFY(!c.authenticated());
        QVERIFY(c.account().isEmpty()); QVERIFY(c.directPeer().isEmpty());
        QVERIFY(c.streams().isEmpty()); QVERIFY(c.messages().isEmpty());
    }

    void anotherProjectDoesNotInheritTheSameUsersPresenceTimestamp()
    {
        const QString otherProject="bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        bool secondProject=false; int usersGets=0;
        Fixture f;
        f.authentication=[&](const Request &request) {
            auto response=sessionResponse(request);
            if(request.target.path()=="/api/core/v1/iam/projects/")
                response.data=QJsonArray{QJsonObject{{"uuid",projectId},{"name","First project"},{"status","ACTIVE"}},
                    QJsonObject{{"uuid",otherProject},{"name","Second project"},{"status","ACTIVE"}}};
            if(secondProject&&request.target.path().endsWith("/actions/introspect")) {
                auto raw=response.data.toObject(); raw["project_id"]=otherProject; response.data=raw;
            }
            return response;
        };
        f.streams=[&](const Request &) {
            return Response{secondProject?QJsonArray{}:QJsonArray{creationChannel(streamId,"First project channel")}};
        };
        f.topics=[&](const Request &) {
            return Response{secondProject?QJsonArray{}:QJsonArray{creationTopic(topicId,streamId,"First project topic")}};
        };
        f.users=[&](const Request &) {
            ++usersGets;
            auto row=presencePerson(userId,secondProject?"second-viewer":"first-viewer",secondProject?"idle":"active",
                secondProject?"2026-01-02T10:00:00Z":"2026-01-02T10:05:00Z");
            row["project_id"]=secondProject?otherProject:projectId;
            return Response{QJsonArray{row}};
        };
        f.controller.login(f.server.origin().toString(),"synthetic-user","synthetic-password","");
        QTRY_COMPARE(f.controller.projects().size(),2); f.controller.openProject(projectId);
        QTRY_COMPARE(f.controller.viewerDisplayName(),QString("first-viewer"));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(usersGets,1);
        secondProject=true;
        // Actual Session public boundary, without controller.reset or a fake row:
        // the new owner may reuse the global user UUID but not its old timestamp.
        f.session.selectProject(otherProject);
        QCOMPARE(f.session.project(),otherProject); QVERIFY(!f.session.ready());
        QVERIFY(f.controller.account().isEmpty());
        QTRY_COMPARE(f.controller.viewerDisplayName(),QString("second-viewer"));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(usersGets,2);
        QCOMPARE(f.controller.account().value("uuid").toString(),userId);
        QCOMPARE(f.controller.account().value("presence").toString(),QString("idle"));
    }

    void avatarImageReferencesUseTheActualUsersCollectionAcrossPublicProjections_data()
    {
        QTest::addColumn<QJsonValue>("wireAvatar");
        QTest::addColumn<QString>("expected");
        QTest::newRow("image") << QJsonValue(avatarReferenceA) << avatarReferenceA;
        QTest::newRow("feff-trim") << QJsonValue(QString(QChar(0xfeff)) + avatarReferenceA + QChar(0xfeff)) << avatarReferenceA;
        QTest::newRow("nel-not-trimmed") << QJsonValue(QString(QChar(0x85)) + avatarReferenceA + QChar(0x85)) << QString{};
        QTest::newRow("nonstring-object") << QJsonValue(QJsonObject{{"image", avatarReferenceA}}) << QString{};
        QTest::newRow("extra-suffix") << QJsonValue(avatarReferenceA + "/extra") << QString{};
        QTest::newRow("nil-uuid-reference") << QJsonValue("urn:image:00000000-0000-0000-0000-000000000000")
            << QString("urn:image:00000000-0000-0000-0000-000000000000");
        const QString upperUuid = "urn:image:" + avatarReferenceA.mid(10).toUpper();
        QTest::newRow("uppercase-uuid") << QJsonValue(upperUuid) << upperUuid;
        QTest::newRow("outer-whitespace") << QJsonValue("  " + avatarReferenceA + "\n") << avatarReferenceA;
        QTest::newRow("absent") << QJsonValue(QJsonValue::Undefined) << QString{};
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << QString{};
        QTest::newRow("invalid-image-id") << QJsonValue("urn:image:not-a-uuid") << QString{};
        QTest::newRow("other-source-urn") << QJsonValue("urn:url:https://images.example.invalid/avatar.png") << QString{};
    }
    void avatarImageReferencesUseTheActualUsersCollectionAcrossPublicProjections()
    {
        QFETCH(QJsonValue, wireAvatar); QFETCH(QString, expected);
        AvatarReferenceFixture test(wireAvatar); auto &c = test.direct.fixture.controller;
        QVERIFY2(test.start(), "Actual Users/direct GET/topic history fixture must settle before avatar oracles");
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets, 1); QCOMPARE(test.direct.gets, 1); QCOMPARE(test.direct.posts, 0);
        QVERIFY(c.directChatDirectoryVerified()); QVERIFY(c.isDirectConversation());
        QCOMPARE(c.selectedStream(), directId); QCOMPARE(c.selectedTopic(), channelDefault);
        QCOMPARE(c.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(c.directPeer().value("displayName").toString(), QString("avatar-peer"));
        QCOMPARE(c.viewerDisplayName(), QString("avatar-self"));
        const auto sidebar = test.sidebar(); QVERIFY(!sidebar.isEmpty());
        QCOMPARE(sidebar.value("peerId").toString(), channelMemberA);
        QCOMPARE(sidebar.value("displayName").toString(), QString("avatar-peer"));
        QCOMPARE(c.channelMemberChoices().first().toMap(), QVariantMap({{"id", channelMemberA},
            {"displayName", "avatar-peer"}, {"username", "avatar-peer"}, {"email", "avatar-peer@example.invalid"}}));
        const auto peer = modelMessage(c, returnedId), own = modelMessage(c, messageId);
        QCOMPARE(peer.value("authorId").toString(), channelMemberA);
        QCOMPARE(peer.value("author").toString(), QString("avatar-peer"));
        QVERIFY(!peer.value("own").toBool()); QVERIFY(own.value("own").toBool());
        QCOMPARE(peer.value("content").toString(), QString("Actual peer image-reference witness"));
        QCOMPARE(c.account().value("uuid").toString(), userId);
        // Proposed additive public display contract; no loader invocation.
        QCOMPARE(c.account().value("avatar").toString(), expected);
        QCOMPARE(c.directPeer().value("avatar").toString(), expected);
        QCOMPARE(sidebar.value("avatar").toString(), expected);
        QCOMPARE(peer.value("authorAvatar").toString(), expected);
        QCOMPARE(own.value("authorAvatar").toString(), expected);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/"));
    }

    void avatarOnlyRealtimeUpdatesNotifyEveryAffectedProjectionAndClearOldReferences()
    {
        AvatarReferenceFixture test(QJsonValue::Null); auto &c = test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto accountBefore = c.account();
        const auto context = c.directChatContextKey();
        const auto names = qMakePair(c.directPeer().value("displayName"), modelMessage(c, returnedId).value("author"));
        QSignalSpy state(&c, &NativeController::stateChanged), streams(&c, &NativeController::streamsChanged),
            messages(&c, &NativeController::messagesChanged);
        QVERIFY(state.isValid() && streams.isValid() && messages.isValid());
        const QList<QJsonValue> values{QJsonValue(avatarReferenceA), QJsonValue(avatarReferenceB),
            QJsonValue::Null, QJsonValue("urn:image:not-a-uuid"), QJsonValue(avatarReferenceA)};
        const QStringList expected{avatarReferenceA, avatarReferenceB, {}, {}, avatarReferenceA};
        int epoch = 30;
        for (qsizetype i = 0; i < values.size(); ++i) {
            const auto beforePeer = c.directPeer().value("avatar").toString();
            const auto beforeViewer = c.account().value("avatar").toString();
            state.clear(); streams.clear(); messages.clear();
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                avatarReferencePerson(channelMemberA, "avatar-peer", values[i]), epoch++)));
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                avatarReferencePerson(userId, "avatar-self", values[i]), epoch++)));
            QCOMPARE(c.directPeer().value("avatar").toString(), expected[i]);
            QCOMPARE(test.sidebar().value("avatar").toString(), expected[i]);
            QCOMPARE(modelMessage(c, returnedId).value("authorAvatar").toString(), expected[i]);
            QCOMPARE(modelMessage(c, messageId).value("authorAvatar").toString(), expected[i]);
            QCOMPARE(c.account().value("avatar").toString(), expected[i]);
            if (beforePeer != expected[i]) { QVERIFY(streams.size() > 0); QVERIFY(messages.size() > 0); }
            if (beforeViewer != expected[i]) QVERIFY(state.size() > 0);
            QCOMPARE(c.directPeer().value("displayName"), names.first);
            QCOMPARE(modelMessage(c, returnedId).value("author"), names.second);
            QCOMPARE(c.account().value("displayName"), accountBefore.value("displayName"));
            QVERIFY(c.directChatDirectoryVerified()); // Malformed avatar is not malformed creator identity.
            QCOMPARE(c.directChatContextKey(), context);
            QCOMPARE(c.selectedStream(), directId); QCOMPARE(c.selectedTopic(), channelDefault);
        }
        auto foreign = controllerEvent("user.updated", avatarReferencePerson(channelMemberA, "avatar-peer", avatarReferenceB), epoch++);
        foreign["user_uuid"] = channelMemberB;
        state.clear(); streams.clear(); messages.clear();
        QVERIFY(!c.applyRealtimeEvent(foreign));
        QCOMPARE(c.directPeer().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(state.size(), 0); QCOMPARE(streams.size(), 0); QCOMPARE(messages.size(), 0);
        QCOMPARE(test.direct.posts, 0); QCOMPARE(test.direct.usersGets, 1);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/"));
        c.logout(); QVERIFY(c.account().isEmpty()); QVERIFY(c.directPeer().isEmpty());
        QVERIFY(c.streams().isEmpty()); QVERIFY(c.messages().isEmpty());
    }

    void avatarDisplayDoesNotInheritAnUnrelatedCreatorChoiceFailure()
    {
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(c.directChatDirectoryVerified());
        auto unrelated = test.directory.at(2).toObject();
        unrelated["first_name"] = QString(129, 'x'); // Existing creator bound; unrelated valid peers remain named.
        test.directory.replace(2, unrelated);
        const auto before = test.direct.usersGets;
        c.refresh(); QTRY_COMPARE(test.direct.usersGets, before + 1);
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_VERIFY(!c.busy());
        QVERIFY(!c.directChatDirectoryVerified()); QVERIFY(!c.channelMembersVerified());
        QVERIFY(!c.canStartDirectChatWith(channelMemberA)); QVERIFY(c.channelMemberChoices().isEmpty());
        QVERIFY(c.isDirectConversation()); QCOMPARE(c.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(c.directPeer().value("displayName").toString(), QString("avatar-peer"));
        QCOMPARE(c.viewerDisplayName(), QString("avatar-self"));
        QCOMPARE(modelMessage(c, returnedId).value("author").toString(), QString("avatar-peer"));
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(c.directPeer().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(test.sidebar().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(modelMessage(c, returnedId).value("authorAvatar").toString(), avatarReferenceA);
        QCOMPARE(test.direct.posts, 0); // Display data did not restore creation authority.
    }

    void delayedUsersFromThePreviousOwnerCannotPublishAvatarReferences()
    {
        // All references captured by HTTP callbacks outlive Fixture destruction.
        bool newOwner = false, holdNext = false;
        int usersGets = 0;
        std::function<void()> releaseOld;
        Fixture f;
        f.users = [&](const Request &) {
            ++usersGets;
            Response response{QJsonArray{avatarReferencePerson(newOwner ? returnedId : userId,
                newOwner ? "new-viewer" : "old-viewer", newOwner ? avatarReferenceB : avatarReferenceA),
                avatarReferencePerson(channelMemberA, newOwner ? "new-peer" : "old-peer",
                    newOwner ? avatarReferenceB : avatarReferenceA)}};
            if (holdNext) { holdNext = false; response.defer = [&](std::function<void()> send) { releaseOld = std::move(send); }; }
            return response;
        };
        f.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (newOwner && request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto user = raw.value("user_info").toObject();
                user["uuid"] = returnedId; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        f.messages = [](const Request &) {
            auto row = message("Actual owner-bound history"); row["author_uuid"] = channelMemberA; row["is_own"] = false;
            return Response{QJsonArray{row}};
        };
        QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.viewerDisplayName(), QString("old-viewer")); QCOMPARE(usersGets, 1);
        holdNext = true; f.controller.refresh(); QTRY_COMPARE(usersGets, 2); QTRY_VERIFY(bool(releaseOld));
        QPointer<QNetworkReply> oldReply;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/") oldReply = reply;
        QVERIFY(oldReply); QVERIFY(!oldReply->isFinished());
        const auto oldGeneration = f.api.generation();
        f.controller.logout();
        QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.controller.messages().isEmpty());
        QTRY_VERIFY(oldReply.isNull() || oldReply->isFinished());
        newOwner = true; QVERIFY(f.start());
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.account().value("uuid").toString(), returnedId);
        QCOMPARE(f.controller.viewerDisplayName(), QString("new-viewer")); QCOMPARE(usersGets, 3);
        QCOMPARE(modelMessage(f.controller, messageId).value("author").toString(), QString("new-peer"));
        QVERIFY(f.api.generation() > oldGeneration);
        QCOMPARE(f.controller.account().value("avatar").toString(), avatarReferenceB);
        QCOMPARE(modelMessage(f.controller, messageId).value("authorAvatar").toString(), avatarReferenceB);
        releaseOld(); releaseOld = {}; // Existing defer writer guards cancelled socket; no raw pointer access.
        QVERIFY(oldReply.isNull() || oldReply->isFinished());
        QCOMPARE(f.controller.viewerDisplayName(), QString("new-viewer"));
        QCOMPARE(f.controller.account().value("avatar").toString(), avatarReferenceB);
        QCOMPARE(modelMessage(f.controller, messageId).value("authorAvatar").toString(), avatarReferenceB);
        auto oldEvent = controllerEvent("user.updated", avatarReferencePerson(channelMemberA, "old-peer", avatarReferenceA), 90);
        QVERIFY(!f.controller.applyRealtimeEvent(oldEvent)); // Envelope belongs to old userId.
        QCOMPARE(modelMessage(f.controller, messageId).value("authorAvatar").toString(), avatarReferenceB);
    }

    void genericMessageAuthorUsesSourceUnknownFallback_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"missing-user", "known-name", "null-optional-names",
                                    "blank-optional-names", "own-missing-user", "own-known-name", "server-own-foreign"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void genericMessageAuthorUsesSourceUnknownFallback()
    {
        QFETCH(QString, scenario);
        const QString peer = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const bool identityOwn = scenario.startsWith("own-");
        const bool serverOwn = scenario == "server-own-foreign";
        const auto author = identityOwn ? userId : peer;
        const bool named = scenario == "known-name" || scenario == "own-known-name";
        const bool usernameOnly = scenario == "null-optional-names" || scenario == "blank-optional-names";
        const QString preferred = "Known peer";
        const QString username = "known-peer";
        QJsonArray directory;
        if (named || usernameOnly) {
            QJsonObject user{{"uuid", author}, {"username", username}, {"source", "iam"}, {"status", "offline"},
                {"last_ping_at", "2026-01-02T10:00:00Z"}, {"created_at", "2026-01-02T10:00:00Z"},
                {"updated_at", "2026-01-02T10:00:00Z"}, {"name", named ? QJsonValue(preferred) : QJsonValue::Null},
                {"display_name", QJsonValue::Null},
                {"first_name", named ? QJsonValue("Known") : scenario == "blank-optional-names" ? QJsonValue(" ") : QJsonValue::Null},
                {"last_name", named ? QJsonValue("peer") : scenario == "blank-optional-names" ? QJsonValue("") : QJsonValue::Null}};
            directory.append(user);
        }
        auto served = message("Actual HTTP author fallback witness");
        served["author_uuid"] = author; served["is_own"] = serverOwn;
        int usersGets = 0, historyGets = 0, mutations = 0;
        Fixture fixture;
        fixture.users = [directory, &usersGets](const Request &) { ++usersGets; return Response{directory}; };
        fixture.messages = [served, &historyGets, &mutations](const Request &request) {
            if (request.method == "GET") ++historyGets; else ++mutations;
            return Response{QJsonArray{served}};
        };
        QVERIFY(fixture.start());
        QTRY_COMPARE(usersGets, 1);
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        const auto row = modelMessage(fixture.controller, messageId);
        QVERIFY(!row.isEmpty()); QVERIFY(historyGets > 0); QCOMPARE(mutations, 0);
        QCOMPARE(row.value("id").toString(), messageId);
        QCOMPARE(row.value("authorId").toString(), author);
        QCOMPARE(row.value("topicId").toString(), topicId);
        QCOMPARE(row.value("kind").toString(), QString("markdown"));
        QCOMPARE(row.value("content").toString(), served.value("payload").toObject().value("content").toString());
        QCOMPARE(row.value("own").toBool(), identityOwn || serverOwn);
        const auto expected = named ? preferred : usernameOnly ? username
            : identityOwn || serverOwn ? NativeController::tr("You") : "#" + author.left(8);
        QCOMPARE(row.value("author").toString(), expected);
    }

    void knownTopicOpeningIgnoresAStaleStreamHintWithoutLookup()
    {
        TopicOpeningFixture test;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        QVERIFY(controller.topicsComplete());
        controller.openTopic(secondTopicId, openingStreamId);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), secondTopicId);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.catalogs, 0);
        QCOMPARE(test.users, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QCOMPARE(test.mutations, 0);
        QVERIFY(!controller.topicResolving());
    }

    void profileShareRejectsInvalidServerUser_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"valid", "uuid", "project", "avatar", "status_text", "date"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }

    void profileShareRejectsInvalidServerUser()
    {
        QFETCH(QString, field);
        Fixture f;
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        QJsonObject row{{"uuid", target}, {"username", "link-user"}, {"first_name", "Link"},
            {"source", "iam"}, {"status", "active"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        if (field == "uuid") row["uuid"] = channelMemberA;
        else if (field == "project") row["project_id"] = secondTopicId;
        else if (field == "avatar") row["avatar"] = 123;
        else if (field == "status_text") row["status_text"] = QJsonObject{};
        else if (field == "date") row["last_ping_at"] = "not-a-date";
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/users/" + target) {
                ++requests; return {row};
            }
            return fallback(request);
        };
        QVERIFY(f.start());
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + target);
        QTRY_COMPARE(requests, 1);
        if (field == "valid") {
            QTRY_COMPARE(opened.size(), 1);
            QCOMPARE(opened.at(0).at(0).toString(), target);
            QVERIFY(!f.controller.profileLinkFailed());
        } else {
            QTRY_VERIFY(f.controller.profileLinkFailed());
            QCOMPARE(opened.size(), 0);
        }
    }

    void profileShareFailureDisappearsOnNavigation()
    {
        Fixture f; f.extraTopic = true;
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000124");
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/users/" + target)
                return {QJsonObject{}, 404};
            return fallback(request);
        };
        QVERIFY(f.start());
        f.controller.openMessageLink("ew://open/#user/" + target);
        QTRY_VERIFY(f.controller.profileLinkFailed());
        f.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(f.controller.selectedTopic(), secondTopicId);
        QVERIFY(!f.controller.profileLinkFailed());
    }

    void profileShareRejectsForeignOrganizationRoute()
    {
        Fixture f;
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target) ++requests;
            return fallback(request);
        };
        QVERIFY(f.start());
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/org/foreign/settings#user/" + target);
        QTest::qWait(500);
        QCOMPARE(requests, 0);
        QCOMPARE(opened.size(), 0);
        QVERIFY(!f.controller.profileLinkFailed());
    }

    void profileShareCacheHitOpensOfflineAndRefreshesOnce()
    {
        Fixture f; QTemporaryDir directory;
        NativeCacheService cache(directory.path(), {}, true);
        f.controller.useCache(&cache);
        QVERIFY(f.start()); QTRY_VERIFY(cache.available()); QVERIFY(flushCache(cache));
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        QVERIFY(cache.put(cache.lease(), {NativeCache::Kind::Users, {}, {}, {}},
            {QJsonObject{{"uuid", target}, {"username", "cached"}, {"first_name", "Cached"}}}));
        QVERIFY(flushCache(cache));
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target) {
                ++requests; return {QJsonObject{}, 503};
            }
            return fallback(request);
        };
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + target);
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), target);
        QCOMPARE(f.controller.profileSnapshot(target).value("title").toString(), QString("Cached"));
        QTRY_COMPARE(requests, 1);
        QVERIFY(!f.controller.profileLinkFailed());
    }

    void profileShareCacheDestructionDoesNotLoseNetworkLookup()
    {
        Fixture f; QTemporaryDir directory;
        QSemaphore release; std::atomic<bool> entered = false, block = false;
        auto cache = std::make_unique<NativeCacheService>(directory.path(), NativeCache::Policy{}, true, [&] {
            if (block.exchange(false)) { entered = true; release.acquire(); }
            return QDateTime::currentMSecsSinceEpoch();
        });
        f.controller.useCache(cache.get());
        QVERIFY(f.start()); QTRY_VERIFY(cache->available()); QVERIFY(flushCache(*cache));
        auto unblock = qScopeGuard([&] { release.release(); });
        block = true;
        QVERIFY(cache->put(cache->lease(), {NativeCache::Kind::Users, {}, {}, {}},
            {QJsonObject{{"uuid", userId}, {"username", "self"}}}));
        QTRY_VERIFY(entered.load());
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target) {
                ++requests;
                return {QJsonObject{{"uuid", target}, {"username", "link-user"}, {"source", "iam"},
                    {"status", "active"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
                    {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}}};
            }
            return fallback(request);
        };
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + target);
        QCOMPARE(requests, 0);
        QTimer::singleShot(100, &f.controller, [&] { release.release(); });
        cache.reset(); unblock.dismiss();
        QTRY_COMPARE_WITH_TIMEOUT(requests, 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 3000);
    }

    void profileShareLateResponseCannotCrossRouteOrSession_data()
    {
        QTest::addColumn<QString>("replacement");
        for (const auto *value : {"link", "selection", "refresh-selection", "logout"}) QTest::newRow(value) << QString::fromLatin1(value);
    }

    void profileShareLateResponseCannotCrossRouteOrSession()
    {
        QFETCH(QString, replacement);
        Fixture f; f.extraTopic = true;
        const QString first = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        const QString second = QStringLiteral("aa000000-0000-0000-0000-000000000124");
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/users/" + first || path == "/api/workspace/v1/messenger/users/" + second) {
                ++requests;
                const auto id = path.endsWith(first) ? first : second;
                return {QJsonObject{{"uuid", id}, {"username", "link-user"}, {"source", "iam"},
                    {"status", "active"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
                    {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}}, 200, id == first ? 700 : 0};
            }
            return fallback(request);
        };
        QVERIFY(f.start());
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + first);
        QTRY_COMPARE(requests, 1);
        if (replacement == "link") {
            f.controller.openMessageLink("ew://open/#user/" + second);
            QTRY_COMPARE(requests, 2);
            QTRY_COMPARE(opened.size(), 1);
            QCOMPARE(opened.at(0).at(0).toString(), second);
        } else if (replacement == "selection" || replacement == "refresh-selection") {
            if (replacement == "refresh-selection") f.controller.refresh();
            f.controller.selectTopic(secondTopicId);
        } else f.controller.logout();
        QTest::qWait(900);
        QCOMPARE(opened.size(), replacement == "link" ? 1 : 0);
        QVERIFY(!f.controller.profileLinkFailed());
    }

    void profileShareBeforeLoginIsConsumedOnce()
    {
        Fixture f;
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target) {
                ++requests;
                return {QJsonObject{{"uuid", target}, {"username", "link-user"}, {"source", "iam"},
                    {"status", "active"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
                    {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}}};
            }
            return fallback(request);
        };
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + target);
        QCOMPARE(requests, 0);
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 5000);
        QCOMPARE(opened.at(0).at(0).toString(), target);
        QCOMPARE(requests, 1);
        f.controller.refresh();
        QTest::qWait(1000);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(requests, 1);
    }

    void profileShareNeverUsesAnotherOwnersCache()
    {
        Fixture f; QTemporaryDir directory;
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        {
            NativeCache seed(directory.path(), {}, true);
            QVERIFY(seed.open(f.server.origin(), secondTopicId, userId));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::Users, {}, {}, {}},
                {QJsonObject{{"uuid", target}, {"username", "foreign"}}}));
        }
        NativeCacheService cache(directory.path(), {}, true);
        f.controller.useCache(&cache);
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target) {
                ++requests; return {QJsonObject{}, 404};
            }
            return fallback(request);
        };
        QVERIFY(f.start()); QTRY_VERIFY(cache.available());
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + target);
        QTRY_COMPARE(requests, 1);
        QTRY_VERIFY(f.controller.profileLinkFailed());
        QCOMPARE(opened.size(), 0);
    }

    void profileShareSurvivesRefreshWithoutNavigation()
    {
        Fixture f;
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target) {
                ++requests;
                return {QJsonObject{{"uuid", target}, {"username", "link-user"}, {"source", "iam"},
                    {"status", "active"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
                    {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}}, 200, 600};
            }
            return fallback(request);
        };
        QVERIFY(f.start());
        QSignalSpy opened(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + target);
        QTRY_COMPARE(requests, 1);
        f.controller.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 3000);
        QCOMPARE(opened.at(0).at(0).toString(), target);
        QCOMPARE(requests, 1);
    }

    void profileShareFailureSurvivesRefreshOnSameRoute()
    {
        Fixture f;
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target)
                return {QJsonObject{}, 404};
            return fallback(request);
        };
        QVERIFY(f.start());
        f.controller.openMessageLink("ew://open/#user/" + target);
        QTRY_VERIFY(f.controller.profileLinkFailed());
        f.controller.refresh();
        QTest::qWait(1000);
        QVERIFY(f.controller.profileLinkFailed());
    }

    void profileShareFetchedUserRemainsAvailableOfflineAfterRestart()
    {
        Fixture f; QTemporaryDir directory;
        NativeCacheService cache(directory.path(), {}, true);
        f.controller.useCache(&cache);
        const QString target = QStringLiteral("aa000000-0000-0000-0000-000000000123");
        bool offline = false; int requests = 0;
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/users/" + target) {
                ++requests;
                if (offline) return {QJsonObject{}, 503};
                return {QJsonObject{{"uuid", target}, {"username", "link-user"}, {"first_name", "Cached link"},
                    {"source", "iam"}, {"status", "active"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
                    {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}}};
            }
            return fallback(request);
        };
        QVERIFY(f.start()); QTRY_VERIFY(cache.available());
        QSignalSpy first(&f.controller, &NativeController::profileLinkOpened);
        f.controller.openMessageLink("ew://open/#user/" + target);
        QTRY_COMPARE(first.size(), 1);
        QVERIFY(flushCache(cache));
        f.controller.useCache(nullptr);
        offline = true;
        NativeController restarted(&f.api, &f.session);
        restarted.useCache(&cache);
        QTRY_VERIFY(cache.available());
        QSignalSpy second(&restarted, &NativeController::profileLinkOpened);
        restarted.openMessageLink("ew://open/#user/" + target);
        QTRY_COMPARE_WITH_TIMEOUT(second.size(), 1, 3000);
        QCOMPARE(restarted.profileSnapshot(target).value("title").toString(), QString("Cached link"));
        QCOMPARE(restarted.profileSnapshot(target).value("createdAt").toString(), QString("2026-01-02T10:00:00Z"));
        QTRY_COMPARE(requests, 2);
        QVERIFY(!restarted.profileLinkFailed());
    }

    // ---- Issue #134 profile drawer content ----

    void profileResolverResolvesTheReferenceBranches()
    {
        const QString peer = channelMemberA;
        const QString dm = directId;
        const QVector<QPair<QString, QString>> streams{{dm, peer}};
        const QHash<QString, QString> defaults{{dm, channelDefault}};
        auto navigation = resolveProfileDirectNavigation(peer, streams, defaults, QString{});
        QCOMPARE(int(navigation.status), int(ProfileDirectNavigation::Status::OpenDefaultTopic));
        QCOMPARE(navigation.streamUuid, dm);
        QCOMPARE(navigation.topicUuid, channelDefault);
        // Already on the direct stream (stream view or any topic): stay put.
        navigation = resolveProfileDirectNavigation(peer, streams, defaults, dm);
        QCOMPARE(int(navigation.status), int(ProfileDirectNavigation::Status::AlreadyOpen));
        // An existing direct stream without a valid default topic resolves missing.
        navigation = resolveProfileDirectNavigation(peer, streams, {}, QString{});
        QCOMPARE(int(navigation.status), int(ProfileDirectNavigation::Status::MissingDefaultTopic));
        navigation = resolveProfileDirectNavigation(peer, streams, {{dm, "not-a-uuid"}}, QString{});
        QCOMPARE(int(navigation.status), int(ProfileDirectNavigation::Status::MissingDefaultTopic));
        // No direct stream for the peer resolves stream-missing (create path).
        navigation = resolveProfileDirectNavigation(channelMemberB, streams, defaults, streamId);
        QCOMPARE(int(navigation.status), int(ProfileDirectNavigation::Status::StreamMissing));
    }

    void profileSnapshotAndDetailsUseTheVerifiedDirectoryRow()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        QVERIFY(controller.profileSnapshot("not-a-uuid").isEmpty());
        const auto other = controller.profileSnapshot(channelMemberA);
        QCOMPARE(other.value("title").toString(), QStringLiteral("Alice Adams"));
        QCOMPARE(other.value("email").toString(), QStringLiteral("alice@example.invalid"));
        QCOMPARE(other.value("username").toString(), QStringLiteral("alice"));
        QCOMPARE(other.value("status").toString(), QStringLiteral("idle"));
        QCOMPARE(other.value("isSelf").toBool(), false);
        const auto own = controller.profileSnapshot(userId);
        QCOMPARE(own.value("title").toString(), QStringLiteral("Cassi Volkova"));
        QCOMPARE(own.value("isSelf").toBool(), true);
        const auto details = controller.profileDetails(channelMemberA, QStringLiteral("en"));
        QCOMPARE(details.size(), 12);
        const auto byId = [&details](const QString &id) {
            for (const auto &row : details) if (row.toMap().value("id").toString() == id) return row.toMap();
            return QVariantMap{};
        };
        QCOMPARE(byId("userId").value("value").toString(), channelMemberA);
        QCOMPARE(byId("userId").value("unavailable").toBool(), false);
        QCOMPARE(byId("email").value("value").toString(), QStringLiteral("alice@example.invalid"));
        QCOMPARE(byId("joined").value("value").toString(), QStringLiteral("Jan 2, 2026"));
        QCOMPARE(byId("phone").value("unavailable").toBool(), true);
        QCOMPARE(byId("phone").value("value").toString(), QStringLiteral("Temporarily not connected"));
        QCOMPARE(byId("birthday").value("unavailable").toBool(), true);
        QCOMPARE(byId("role").value("unavailable").toBool(), true);
        // Identity from the IAM row: the self snapshot resolves to the session user.
        QCOMPARE(controller.profileDetails(QString{}, QStringLiteral("en")).size(), 0);
    }

    void profileShareLinkFollowsTheReferenceBuilder()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        const auto origin = test.fixture.server.origin().toString();
        QVERIFY(!origin.endsWith(QLatin1Char('/')));
        QCOMPARE(controller.buildProfileShareLink(userId), origin + "/#user/" + userId);
        // Invalid identities and origins never produce a share link.
        QCOMPARE(controller.buildProfileShareLink("not-a-uuid"), QString{});
        QCOMPARE(controller.buildProfileShareLink(QString{}), QString{});
    }

    void ownProfileLoadPopulatesTheBaselineAndRecoverableFailuresRetry()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        QCOMPARE(controller.ownProfileBusy(), false);
        controller.loadOwnProfile();
        QTRY_VERIFY(!controller.ownProfileBusy());
        QCOMPARE(controller.ownProfileLoaded(), true);
        const auto profile = controller.ownProfile();
        QCOMPARE(profile.value("firstName").toString(), QStringLiteral("Cassi"));
        QCOMPARE(profile.value("lastName").toString(), QStringLiteral("Volkova"));
        QCOMPARE(profile.value("phone").toString(), QString{});
        QCOMPARE(profile.value("description").toString(), QStringLiteral("Hello"));
        QCOMPARE(controller.ownProfileError(), QString{});
        QCOMPARE(test.iamGets, 1);
        // A response whose identity does not match the session user is rejected.
        test.iamResponse["uuid"] = channelMemberA;
        controller.loadOwnProfile();
        QTRY_VERIFY(!controller.ownProfileBusy());
        QCOMPARE(controller.ownProfileLoaded(), true); // baseline kept, error reported
        QCOMPARE(controller.ownProfileError(), QStringLiteral("Could not load profile. Try again."));
    }

    void ownProfileSaveSendsOnlyChangedKeysAndRefreshesTheProjection()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        controller.loadOwnProfile();
        QTRY_VERIFY(controller.ownProfileLoaded());
        const QSignalSpy settled(&controller, &NativeController::ownProfileSettledChanged);
        QVERIFY(settled.isValid());
        controller.saveOwnProfile({{"firstName", "Cassi"}, {"lastName", "Volkova"},
            {"phone", " +1 555 0100 "}, {"description", "Hello"}}, QString{}, QString{});
        QTRY_VERIFY(!controller.ownProfileSaving());
        QTRY_COMPARE(settled.size(), 1);
        QCOMPARE(controller.ownProfileError(), QString{});
        QCOMPARE(test.iamPuts, 1);
        // Only the changed keys are sent and names are trimmed.
        const QJsonObject expectedPatch{{"phone", "+1 555 0100"}};
        QCOMPARE(test.putBodies.first(), expectedPatch);
        QTRY_COMPARE(test.directoryGets, 1);
        QCOMPARE(controller.ownProfileRefreshPending(), false);
        // A cleared name becomes null in the patch (reference computePatch).
        controller.saveOwnProfile({{"firstName", "  "}}, QString{}, QString{});
        QTRY_VERIFY(!controller.ownProfileSaving());
        const QJsonObject expectedNullName{{"first_name", QJsonValue::Null}};
        QCOMPARE(test.putBodies.last(), expectedNullName);
    }

    void ownProfileSaveFailureKeepsTheEditorOpenWithAMappedError_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("message");
        QTest::newRow("transient") << 500 << QStringLiteral("Failed to update profile");
        QTest::newRow("forbidden") << 403 << QStringLiteral("Your server does not allow this profile request.");
        QTest::newRow("unsupported") << 404 << QStringLiteral("Profile editing is not available yet");
        QTest::newRow("invalid") << 400 << QStringLiteral("Check the lengths of the editable fields.");
    }
    void ownProfileSaveFailureKeepsTheEditorOpenWithAMappedError()
    {
        QFETCH(int, status); QFETCH(QString, message);
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        controller.loadOwnProfile();
        QTRY_VERIFY(controller.ownProfileLoaded());
        test.iamPutStatus = status;
        controller.saveOwnProfile({{"description", "Changed"}}, QString{}, QString{});
        QTRY_VERIFY(!controller.ownProfileSaving());
        QCOMPARE(controller.ownProfileError(), message);
    }

    void ownProfileRefreshFailureMarksRefreshPendingWithoutRollback()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        controller.loadOwnProfile();
        QTRY_VERIFY(controller.ownProfileLoaded());
        test.directoryStatus = 503;
        controller.saveOwnProfile({{"description", "Saved anyway"}}, QString{}, QString{});
        QTRY_VERIFY(!controller.ownProfileSaving());
        QCOMPARE(controller.ownProfileError(), QString{});
        QTRY_VERIFY(controller.ownProfileRefreshPending());
        // A later successful projection clears the pending marker.
        test.directoryStatus = 200;
        controller.saveOwnProfile({{"description", "Saved again"}}, QString{}, QString{});
        QTRY_VERIFY(!controller.ownProfileSaving());
        QTRY_VERIFY(!controller.ownProfileRefreshPending());
    }

    void ownProfileAvatarUploadIsMultipartAndUpdatesTheDirectory()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("avatar.png");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("89504e470d0a1a0a") + "PNGBYTES-1234"); file.close();
        QCOMPARE(controller.userAvatar(userId), QString{});
        controller.uploadOwnProfileAvatar(path);
        QCOMPARE(controller.ownAvatarBusy(), true);
        QTRY_VERIFY(!controller.ownAvatarBusy());
        QCOMPARE(test.avatarPosts, 1);
        QCOMPARE(controller.ownAvatarError(), QString{});
        QVERIFY(test.multipartBody.contains("name=\"file\""));
        QVERIFY(test.multipartBody.contains("filename=\"avatar.png\""));
        QVERIFY(test.multipartBody.contains("PNGBYTES-1234"));
        QVERIFY(test.multipartContentType.startsWith("multipart/form-data"));
        QCOMPARE(controller.userAvatar(userId), test.avatarRef);
        QCOMPARE(controller.profileSnapshot(userId).value("avatar").toString(), test.avatarRef);
    }

    void ownProfileDelayedRefreshPreservesNewerAvatar_data()
    {
        QTest::addColumn<bool>("remove");
        QTest::newRow("upload") << false;
        QTest::newRow("remove") << true;
    }
    void ownProfileDelayedRefreshPreservesNewerAvatar()
    {
        QFETCH(bool, remove);
        ProfileFixture test;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.loadOwnProfile();
        QTRY_VERIFY(controller.ownProfileLoaded());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("avatar.png");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("89504e470d0a1a0a") + "PNGBYTES");
        file.close();
        if (remove) {
            controller.uploadOwnProfileAvatar(path);
            QTRY_COMPARE(controller.userAvatar(userId), test.avatarRef);
        }
        test.iamResponse["first_name"] = QStringLiteral("Updated");
        test.directoryRow = test.iamResponse;
        test.directoryRow["avatar"] = remove ? QJsonValue(test.avatarRef) : QJsonValue(QJsonValue::Null);
        std::function<void()> releaseRefresh;
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (request.method == "GET"
                && request.target.path() == "/api/workspace/v1/messenger/users/" + userId) {
                ++test.directoryGets;
                Response response{test.directoryRow};
                response.defer = [&](auto send) { releaseRefresh = std::move(send); };
                return response;
            }
            return fallback(request);
        };
        controller.saveOwnProfile({{"firstName", "Updated"}}, remove ? "remove" : "upload", path);
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(releaseRefresh), 2000);
        QTRY_VERIFY(!controller.ownAvatarBusy());
        const auto expected = remove ? QString{} : test.avatarRef;
        QCOMPARE(controller.userAvatar(userId), expected);
        releaseRefresh();
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(controller.profileSnapshot(userId).value("title").toString(), QStringLiteral("Updated Volkova"));
        QCOMPARE(controller.profileSnapshot(userId).value("avatar").toString(), expected);
        QVERIFY(!controller.ownProfileRefreshPending());
    }

    void ownProfileAvatarValidationRejectsWrongKindsAndEmptyFiles()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto textPath = directory.filePath("notes.txt");
        QFile text(textPath); QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("not an image"); text.close();
        controller.uploadOwnProfileAvatar(textPath);
        QCOMPARE(controller.ownAvatarError(), QStringLiteral("Please choose a valid image file"));
        QCOMPARE(test.avatarPosts, 0);
        const auto emptyPath = directory.filePath("empty.png");
        QFile empty(emptyPath); QVERIFY(empty.open(QIODevice::WriteOnly)); empty.close();
        controller.uploadOwnProfileAvatar(emptyPath);
        QCOMPARE(controller.ownAvatarError(), QStringLiteral("Please choose a valid image file"));
        QCOMPARE(test.avatarPosts, 0);
        const auto bigPath = directory.filePath("big.png");
        QFile big(bigPath); QVERIFY(big.open(QIODevice::WriteOnly));
        big.resize(25LL * 1024 * 1024 + 1); big.close();
        controller.uploadOwnProfileAvatar(bigPath);
        QCOMPARE(controller.ownAvatarError(), QStringLiteral("Image is too large (max 25 MB)"));
        QCOMPARE(test.avatarPosts, 0);
        const auto caps = controller.profileAvatarCapabilities();
        QCOMPARE(caps.value("maxAvatarFileSizeMib").toInt(), 25);
        QCOMPARE(caps.value("avatarChangesDisabled").toBool(), false);
    }

    void ownProfileAvatarResetAndServerFailuresMapTheReferenceErrors_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("message");
        QTest::newRow("forbidden") << 403 << QStringLiteral("Avatar changes are disabled by server settings");
        QTest::newRow("unsupported") << 404 << QStringLiteral("Avatar update is not supported by this server");
        QTest::newRow("invalid") << 415 << QStringLiteral("Please choose a valid image file");
        QTest::newRow("transient") << 500 << QStringLiteral("Failed to update avatar");
    }
    void ownProfileAvatarResetAndServerFailuresMapTheReferenceErrors()
    {
        QFETCH(int, status); QFETCH(QString, message);
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        test.avatarStatus = status;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("avatar.png");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("89504e470d0a1a0a") + "PNGBYTES"); file.close();
        controller.uploadOwnProfileAvatar(path);
        QTRY_VERIFY(!controller.ownAvatarBusy());
        QCOMPARE(controller.ownAvatarError(), message);
        // A successful reset clears the avatar reference and settles.
        test.avatarStatus = 200;
        const QSignalSpy settled(&controller, &NativeController::ownAvatarSettledChanged);
        QVERIFY(settled.isValid());
        controller.removeOwnProfileAvatar();
        QTRY_VERIFY(!controller.ownAvatarBusy());
        QCOMPARE(test.resetPosts, 1);
        QCOMPARE(test.fixture.server.requests.last().body, QJsonObject{});
        QCOMPARE(controller.ownAvatarError(), QString{});
        QTRY_COMPARE(settled.size(), 1);
        QCOMPARE(controller.userAvatar(userId), QString{});
    }

    void openDirectChatFromProfileOpensExistingDefaultTopic()
    {
        DirectChatFixture test; test.seed(); QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        // Populate the catalog with the existing direct stream via the lookup.
        controller.startDirectChat(channelMemberA);
        QTRY_COMPARE(controller.selectedStream(), directId);
        controller.selectStream(streamId);
        QTRY_VERIFY(!controller.busy());
        const auto topicGets = test.topicGets;
        controller.openDirectChatFromProfile(channelMemberA);
        QTRY_COMPARE(controller.selectedStream(), directId);
        QTRY_COMPARE(controller.selectedTopic(), channelDefault);
        QCOMPARE(test.posts, 0); // resolved via lookup, never a create
        QVERIFY(test.topicGets > topicGets);
        // Already on the direct stream (any topic): a repeat request changes nothing.
        controller.openDirectChatFromProfile(channelMemberA);
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedStream(), directId);
        QCOMPARE(controller.selectedTopic(), channelDefault);
        QCOMPARE(controller.profileDirectPending(), false);
    }

    void profileMessageResolvesOmittedDefaultFromVerifiedTopics_data()
    {
        QTest::addColumn<int>("defaultCount");
        QTest::addColumn<bool>("staleCache");
        QTest::newRow("one-default") << 1 << false;
        QTest::newRow("no-default") << 0 << false;
        QTest::newRow("failed-list") << -1 << false;
        QTest::newRow("ambiguous-default") << 2 << false;
        QTest::newRow("stale-default-awaits-verification") << 1 << true;
    }
    void profileMessageResolvesOmittedDefaultFromVerifiedTopics()
    {
        QFETCH(int, defaultCount);
        QFETCH(bool, staleCache);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), NativeCache::Policy{}, true);
        DirectChatFixture test;
        if (staleCache) test.fixture.controller.useCache(&cache);
        test.seed();
        test.created.remove("default_topic_uuid");
        test.gets = 1; // Include the existing DM in the initial stream catalog.
        std::function<void()> releaseTopics;
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/stream_topics/")
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == directId) {
                ++test.topicGets;
                QJsonArray rows;
                for (int i = 0; i < defaultCount; ++i) {
                    auto row = test.defaultTopic;
                    if (i) row["uuid"] = QStringLiteral("88000000-0000-4000-8000-000000000011");
                    rows.append(row);
                }
                Response response{rows, defaultCount < 0 ? 503 : 200};
                response.defer = [&](auto send) { releaseTopics = std::move(send); };
                return response;
            }
            return fallback(request);
        };
        auto &controller = test.fixture.controller;
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 2);
        QTRY_VERIFY(!controller.busy());
        if (staleCache) {
            QTRY_VERIFY(cache.available());
            QVERIFY(flushCache(cache));
            QVERIFY(cache.put(cache.lease(), {NativeCache::Kind::Topics, directId, {}, {}},
                QJsonArray{test.defaultTopic}, {}, nullptr, {}, true));
            QVERIFY(flushCache(cache));
            const auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, directId, {}, {}});
            QVERIFY(saved && !saved->fresh);
        }
        controller.openDirectChatFromProfile(channelMemberA);
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(releaseTopics), 2000);
        QCOMPARE(test.posts, 0);
        QCOMPARE(controller.selectedStream(), directId);
        QCOMPARE(controller.selectedTopic(), QString{});
        QVERIFY(controller.profileDirectPending());
        releaseTopics();
        QTRY_VERIFY(!controller.profileDirectPending());
        QCOMPARE(controller.selectedTopic(), defaultCount == 1 ? channelDefault : QString{});
    }

    void profileMessageDeferredCacheWaitsForNetwork_data()
    {
        QTest::addColumn<QString>("terminal");
        for (const auto *terminal : {"success", "failure", "logout"})
            QTest::newRow(terminal) << QString(terminal);
    }

    void profileMessageDeferredCacheWaitsForNetwork()
    {
        QFETCH(QString, terminal);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        DirectChatFixture test; test.seed(); test.created.remove("default_topic_uuid"); test.gets = 1;
        {
            NativeCache seed(directory.path(), {}, true);
            QVERIFY(seed.open(test.fixture.server.origin(), projectId, userId));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::Topics, directId, {}, {}}, {test.defaultTopic}));
        }
        QSemaphore gate; std::atomic_bool hold{false}, entered{false};
        NativeCacheService cache(directory.path(), {}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.acquire(); }
            return QDateTime::currentMSecsSinceEpoch();
        });
        QVERIFY(cache.open(test.fixture.server.origin(), projectId, userId));
        QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 3000);
        hold = true;
        cache.get(cache.lease(), {NativeCache::Kind::Topics, directId, {}, {}}, this, [](auto) {});
        const auto release = qScopeGuard([&] { gate.release(); });
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
        auto &controller = test.fixture.controller;
        controller.useCache(&cache);
        std::function<void()> releaseTopics;
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/stream_topics/")
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == directId) {
                Response response{QJsonArray{test.defaultTopic}, terminal == "failure" ? 503 : 200};
                response.defer = [&](auto send) { releaseTopics = std::move(send); };
                return response;
            }
            return fallback(request);
        };
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 2); QTRY_VERIFY(!controller.busy());
        QVERIFY(!cache.available());
        controller.openDirectChatFromProfile(channelMemberA);
        QTRY_VERIFY_WITH_TIMEOUT(bool(releaseTopics), 3000);
        QVERIFY(controller.profileDirectPending());
        gate.release(); QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topics().isEmpty(), 3000);
        QVERIFY(controller.profileDirectPending());
        if (terminal == "logout") {
            controller.logout();
            QTRY_VERIFY(!controller.authenticated());
        }
        releaseTopics();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.profileDirectPending(), 3000);
        if (terminal == "success") QTRY_COMPARE(controller.selectedTopic(), channelDefault);
        else QVERIFY(controller.selectedTopic().isEmpty());
    }

    void profileMessageLateDefaultCannotReplaceNewerNavigation()
    {
        DirectChatFixture test;
        test.seed();
        test.created.remove("default_topic_uuid");
        test.gets = 1;
        std::function<void()> releaseTopics;
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/stream_topics/")
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == directId) {
                Response response{QJsonArray{test.defaultTopic}};
                response.defer = [&](auto send) { releaseTopics = std::move(send); };
                return response;
            }
            return fallback(request);
        };
        auto &controller = test.fixture.controller;
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 2);
        QTRY_VERIFY(!controller.busy());
        controller.openDirectChatFromProfile(channelMemberA);
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(releaseTopics), 2000);
        controller.selectStream(streamId);
        QTRY_VERIFY(!controller.busy());
        QVERIFY(!controller.profileDirectPending());
        releaseTopics();
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), QString{});
    }

    void openDirectChatFromProfileCreatesMissingStreamAndIgnoresBrokenDefaults()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        const QSignalSpy opened(&controller, &NativeController::directChatOpened);
        QVERIFY(opened.isValid());
        controller.openDirectChatFromProfile(channelMemberA);
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(controller.selectedStream(), directId);
        QCOMPARE(test.posts, 1);
        // The session user and invalid identities are rejected without requests.
        controller.openDirectChatFromProfile(userId);
        controller.openDirectChatFromProfile("not-a-uuid");
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedStream(), directId);
        QCOMPARE(test.posts, 1);
    }

    // exordy #199 review 4553884681: the ticket guard must compare each callback
    // with the current ticket for its operation. A delayed profile load response
    // may never overwrite the baseline that a newer load has installed.
    void ownProfileStaleLoadCannotOverwriteNewerBaseline()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        std::function<void()> releaseStale;
        bool holdFirstIamGet = true;
        const auto chained = test.fixture.server.handler;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (holdFirstIamGet && request.method == "GET"
                && request.target.path() == "/api/core/v1/iam/users/" + userId) {
                holdFirstIamGet = false;
                auto stale = test.iamResponse;
                stale["first_name"] = QStringLiteral("Stale");
                Response response{stale, 200};
                response.defer = [&](std::function<void()> send) { releaseStale = std::move(send); };
                return response;
            }
            return chained(request);
        };
        controller.loadOwnProfile(); // held response, ticket N
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(releaseStale), 2000);
        QVERIFY(controller.ownProfileBusy());
        test.iamResponse["first_name"] = QStringLiteral("Fresh");
        controller.loadOwnProfile(); // newer load completes, ticket N + 1
        QTRY_VERIFY(controller.ownProfileLoaded());
        QCOMPARE(controller.ownProfile().value("firstName").toString(), QStringLiteral("Fresh"));
        // Releasing the stale response must not roll the baseline back.
        releaseStale(); releaseStale = nullptr;
        QTest::qWait(200);
        QCOMPARE(controller.ownProfile().value("firstName").toString(), QStringLiteral("Fresh"));
        // The baseline is still the fresh row: saving a matching draft has no diff.
        controller.saveOwnProfile({{"firstName", "Fresh"}}, QString{}, QString{});
        QTRY_VERIFY(!controller.ownProfileSaving());
        QCOMPARE(test.iamPuts, 0);
    }

    // exordy #199 review 4553884702: validation must read the file contents,
    // not trust the extension. A renamed text file must never hit the wire.
    void ownProfileAvatarValidationChecksMagicBytesNotExtensions()
    {
        ProfileFixture test; QVERIFY(test.start()); auto &controller = test.fixture.controller;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto fakePng = directory.filePath("fake.png");
        QFile fake(fakePng); QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("this is definitely not a png"); fake.close();
        controller.uploadOwnProfileAvatar(fakePng);
        QCOMPARE(controller.ownAvatarError(), QStringLiteral("Please choose a valid image file"));
        QCOMPARE(test.avatarPosts, 0);
        // The magic bytes must also match the type the extension claims.
        const auto gifNamedPng = directory.filePath("confused.png");
        QFile gif(gifNamedPng); QVERIFY(gif.open(QIODevice::WriteOnly));
        gif.write(QByteArray::fromHex("474946383961") + "more gif bytes"); gif.close();
        controller.uploadOwnProfileAvatar(gifNamedPng);
        QCOMPARE(controller.ownAvatarError(), QStringLiteral("Please choose a valid image file"));
        QCOMPARE(test.avatarPosts, 0);
        // A real PNG signature passes and uploads.
        const auto realPng = directory.filePath("real.png");
        QFile png(realPng); QVERIFY(png.open(QIODevice::WriteOnly));
        png.write(QByteArray::fromHex("89504e470d0a1a0a") + "png bytes"); png.close();
        controller.uploadOwnProfileAvatar(realPng);
        QTRY_VERIFY(!controller.ownAvatarBusy());
        QCOMPARE(controller.ownAvatarError(), QString{});
        QCOMPARE(test.avatarPosts, 1);
    }

    // Issue #89 (docs/NATIVE_DESKTOP_CHAT_HEADER_COUNTERS.md): the header
    // projection counts bound members and reports those whose directory
    // status is active as online (messenger-chat-header.lib.ts:94-130).
    void chatHeaderCountersProjectMembersOnlineAndPresence()
    {
        Fixture f;
        const QString otherStream = QStringLiteral("8e0d0000-0000-4000-8000-000000000009");
        auto userRow = [](const QString &id, const QString &username, const QString &status) {
            return QJsonObject{{"uuid", id}, {"username", username}, {"first_name", username},
                {"email", username + "@example.invalid"}, {"status", status},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        QJsonArray directory{userRow(userId, "cassi", "active"),
            userRow(channelMemberA, "alice", "active"),
            userRow(channelMemberB, "bob", "idle")};
        f.users = [&directory](const Request &) { return Response{directory}; };
        auto binding = [](const QString &id, const QString &user, const QString &stream) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream},
                {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"},
                {"notification_mode", "all_messages"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        f.streams = [&otherStream](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}},
                QJsonObject{{"uuid", otherStream}, {"project_id", projectId},
                    {"name", "Solo channel"}, {"encryption", false}}}};
        };
        f.bindings = [&binding, &otherStream](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == otherStream)
                return Response{QJsonArray{binding("61000000-0000-4000-8000-000000000009", userId, otherStream)}};
            return Response{QJsonArray{binding("61000000-0000-4000-8000-000000000001", userId, streamId),
                binding("61000000-0000-4000-8000-000000000002", channelMemberB, streamId)}};
        };
        QVERIFY(f.start({}, 2));
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 0);
        f.controller.refreshChatHeaderMembers();
        // 2 members, 1 online: the viewer is active, bob is idle; bob's
        // idle status is never online (selectOnlineUserCount).
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 1);
        // A presence projection change moves the online half.
        auto flip = directory[2].toObject();
        flip["status"] = "active";
        flip["updated_at"] = "2026-01-02T10:01:00Z";
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("user.updated", flip, 200)));
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 2);
        // Selection changes never leak stale counts: the singular stream
        // reads 0 first, then its own 1/1.
        f.controller.selectStream(otherStream);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 0);
        f.controller.refreshChatHeaderMembers();
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 1);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 1);
        // An empty selection clears the projection again.
        f.controller.selectStream(QString{});
        f.controller.refreshChatHeaderMembers();
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 0);
    }

    // Issue #89 round 2 (PR #432 review): a binding change on the selected
    // stream must move the header caption counts; the selection/auth
    // triggers alone left both counts on the pre-change member set.
    void chatHeaderCountersFollowSelectedStreamBindingEvents()
    {
        Fixture f;
        const QString otherStream = QStringLiteral("8e0d0000-0000-4000-8000-000000000009");
        auto userRow = [](const QString &id, const QString &username, const QString &status) {
            return QJsonObject{{"uuid", id}, {"username", username}, {"first_name", username},
                {"email", username + "@example.invalid"}, {"status", status},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        QJsonArray directory{userRow(userId, "cassi", "active"),
            userRow(channelMemberA, "alice", "active"),
            userRow(channelMemberB, "bob", "idle")};
        f.users = [&directory](const Request &) { return Response{directory}; };
        auto binding = [](const QString &id, const QString &user, const QString &stream) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream},
                {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"},
                {"notification_mode", "all_messages"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        f.streams = [&otherStream](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}, {"role", "owner"}},
                QJsonObject{{"uuid", otherStream}, {"project_id", projectId},
                    {"name", "Solo channel"}, {"encryption", false}}}};
        };
        QJsonArray bound{binding("61000000-0000-4000-8000-000000000001", userId, streamId),
            binding("61000000-0000-4000-8000-000000000002", channelMemberB, streamId)};
        f.bindings = [&bound, &binding, &otherStream](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == otherStream)
                return Response{QJsonArray{binding("61000000-0000-4000-8000-000000000009", userId, otherStream)}};
            return Response{bound};
        };
        QVERIFY(f.start({}, 2));
        f.controller.refreshChatHeaderMembers();
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 1);
        // Adding a member live (stream_bindings.created for the selected
        // stream) moves both halves: alice joins and she is active.
        bound.append(binding("61000000-0000-4000-8000-000000000003", channelMemberA, streamId));
        auto createdPayload = QJsonObject{{"uuid", streamId}, {"items", QJsonArray{bound.last().toObject()}}};
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream_bindings.created", createdPayload)));
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 3);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 2);
        // Removing a member (stream_binding.deleted) moves them back.
        bound.removeLast();
        auto removedPayload = QJsonObject{{"uuid", "61000000-0000-4000-8000-000000000003"},
            {"stream_uuid", streamId}, {"user_uuid", channelMemberA}};
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream_binding.deleted", removedPayload)));
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 1);
        // A binding change on another stream never moves these counts.
        auto otherPayload = QJsonObject{{"uuid", "61000000-0000-4000-8000-000000000009"},
            {"stream_uuid", otherStream}, {"user_uuid", channelMemberB}};
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream_binding.deleted", otherPayload)));
        QTest::qWait(250);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 2);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 1);
    }

    // Issue #89 round 2: the channel info drawer's own member mutations are
    // binding changes on the selected stream too; the header caption must
    // not keep counting the pre-mutation set afterwards.
    void chatHeaderCountersFollowChannelInfoMemberMutations()
    {
        Fixture f;
        auto userRow = [](const QString &id, const QString &username, const QString &status) {
            return QJsonObject{{"uuid", id}, {"username", username}, {"first_name", username},
                {"email", username + "@example.invalid"}, {"status", status},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        QJsonArray directory{userRow(userId, "cassi", "active"),
            userRow(channelMemberA, "alice", "active"),
            userRow(channelMemberB, "bob", "idle")};
        f.users = [&directory](const Request &) { return Response{directory}; };
        auto binding = [](const QString &id, const QString &user) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"},
                {"notification_mode", "all_messages"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        f.streams = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}, {"role", "owner"}}}};
        };
        QJsonArray bound{binding("61000000-0000-4000-8000-000000000001", userId),
            binding("61000000-0000-4000-8000-000000000002", channelMemberB)};
        f.bindings = [&bound](const Request &) { return Response{bound}; };
        const auto fallback = f.server.handler;
        f.server.handler = [&bound, &binding, &fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "DELETE" && path.startsWith("/api/workspace/v1/messenger/stream_bindings/")) {
                const auto uuid = path.section('/', -1);
                for (int index = 0; index < bound.size(); ++index)
                    if (bound[index].toObject().value("uuid").toString() == uuid) { bound.removeAt(index); break; }
                return Response{QJsonObject{}, 204};
            }
            if (request.method == "POST" && path.endsWith("/actions/add_users/invoke")) {
                QJsonArray created;
                const auto members = request.body.value("member").toArray();
                for (const auto &value : members) {
                    const auto row = binding("61000000-0000-4000-8000-000000000003", value.toString());
                    created.append(row);
                    bound.append(row);
                }
                return Response{created, 201};
            }
            return fallback(request);
        };
        QVERIFY(f.start({}, 1));
        f.controller.refreshChatHeaderMembers();
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 1);
        // Open the drawer's member list, then mutate through it.
        f.controller.refreshChannelInfo();
        QTRY_VERIFY(!f.controller.channelInfoLoading());
        QCOMPARE(f.controller.channelInfoMemberCount(), 2);
        // The owner adds alice: 3 members, alice is active.
        f.controller.addChannelInfoMembers(QStringList{channelMemberA});
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 3);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 2);
        // And removes bob's binding: 2 members, both now active.
        f.controller.removeChannelInfoMember("61000000-0000-4000-8000-000000000002");
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 2);
    }

    // Issue #89 round 3 (issue #434): the read path pins zero extra
    // stream_bindings GETs per realtime binding event on the selected
    // stream, so the header projection must adjust locally instead of
    // reloading, and a member/role refresh that does not move the member or
    // online values must not re-notify stateChanged at all.
    void chatHeaderCountersDoNotReNotifyOnSelectedStreamRoleUpdates()
    {
        Fixture f;
        int bindingRequests = 0;
        auto userRow = [](const QString &id, const QString &username, const QString &status) {
            return QJsonObject{{"uuid", id}, {"username", username}, {"first_name", username},
                {"email", username + "@example.invalid"}, {"status", status},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        QJsonArray directory{userRow(userId, "cassi", "active"),
            userRow(channelMemberA, "alice", "active"),
            userRow(channelMemberB, "bob", "idle")};
        f.users = [&directory](const Request &) { return Response{directory}; };
        auto binding = [](const QString &id, const QString &user, const QString &stream) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream},
                {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"},
                {"notification_mode", "all_messages"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        const QString otherStream = QStringLiteral("8e0d0000-0000-4000-8000-000000000009");
        f.streams = [&otherStream](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}, {"role", "owner"}},
                QJsonObject{{"uuid", otherStream}, {"project_id", projectId},
                    {"name", "Solo channel"}, {"encryption", false}}}};
        };
        QJsonArray bound{binding("61000000-0000-4000-8000-000000000001", userId, streamId),
            binding("61000000-0000-4000-8000-000000000002", channelMemberB, streamId)};
        f.bindings = [&bound, &bindingRequests, &streamId](const Request &request) {
            ++bindingRequests;
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            return Response{stream == streamId ? bound : QJsonArray{}};
        };
        QVERIFY(f.start({}, 2));
        f.controller.refreshChatHeaderMembers();
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 1);
        const auto requests = bindingRequests;
        // Every applied realtime event already re-notifies stateChanged once
        // at the end of applyRealtimeEvent (pre-existing master behavior);
        // the pinned contract is that a membership-neutral binding event
        // adds NO extra emission and NO extra stream_bindings GET.
        QSignalSpy changes(&f.controller, &NativeController::stateChanged); QVERIFY(changes.isValid());
        // A foreign member's demotion keeps membership and presence: it must
        // neither move the counts nor fetch the binding list again.
        auto foreign = bound.last().toObject(); foreign["role"] = "moderator";
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream_binding.updated", foreign, 13)));
        QCOMPARE(changes.count(), 1);
        // The read path's own-binding authority refresh (#434's issuer-role
        // reproduction) is likewise membership-neutral for the caption.
        auto own = bound.first().toObject(); own["role"] = "administrator";
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream_binding.updated", own, 14)));
        QTest::qWait(250);
        QCOMPARE(changes.count(), 2);
        QCOMPARE(bindingRequests, requests);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 2);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 1);
    }

    // Issue #89 round 3 (PR #432 review finding 4628746243): a realtime
    // binding event that arrives while a member page load is in flight must
    // survive the older page result. The held (deferred) page snapshot is
    // the exact race the in-place event update used to lose: completion
    // replaced the member set with the pre-event response.
    void chatHeaderCountersReconcileBindingEventsDuringPageLoad()
    {
        Fixture f;
        int bindingRequests = 0;
        bool holdNext = false;
        std::function<void()> release;
        auto userRow = [](const QString &id, const QString &username, const QString &status) {
            return QJsonObject{{"uuid", id}, {"username", username}, {"first_name", username},
                {"email", username + "@example.invalid"}, {"status", status},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        QJsonArray directory{userRow(userId, "cassi", "active"),
            userRow(channelMemberA, "alice", "active"),
            userRow(channelMemberB, "bob", "idle")};
        f.users = [&directory](const Request &) { return Response{directory}; };
        auto binding = [](const QString &id, const QString &user) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"},
                {"notification_mode", "all_messages"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        f.streams = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}, {"role", "owner"}}}};
        };
        QJsonArray bound{binding("61000000-0000-4000-8000-000000000001", userId),
            binding("61000000-0000-4000-8000-000000000002", channelMemberB)};
        f.bindings = [&bound, &bindingRequests, &holdNext, &release](const Request &) {
            ++bindingRequests;
            // The Response copies the member set at request time: once held,
            // it is a stale snapshot no matter what the server state becomes.
            Response response{bound};
            if (holdNext) {
                holdNext = false;
                response.defer = [&release](std::function<void()> send) { release = std::move(send); };
            }
            return response;
        };
        QVERIFY(f.start({}, 1));
        f.controller.refreshChatHeaderMembers();
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QTRY_COMPARE(f.controller.chatHeaderOnlineCount(), 1);
        const auto baselineRequests = bindingRequests;
        // Phase 1: alice's creation event lands while a page snapshot that
        // predates it is still in flight.
        holdNext = true;
        f.controller.refreshChatHeaderMembers();
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(release), 5000);
        bound.append(binding("61000000-0000-4000-8000-000000000003", channelMemberA));
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream_bindings.created",
            QJsonObject{{"uuid", streamId}, {"items", QJsonArray{bound.last().toObject()}}}, 301)));
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 3); // in-place event update
        QSignalSpy changes(&f.controller, &NativeController::stateChanged); QVERIFY(changes.isValid());
        release(); // the older snapshot arrives after the event
        QTest::qWait(250);
        // The completion must reconcile with the event, never resurrect the
        // pre-event member set (finding 4628746243).
        QCOMPARE(f.controller.chatHeaderMemberCount(), 3);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 2);
        // The reconciled set equals what was already displayed: no re-notify
        // and no extra stream_bindings GET (round 2 properties hold).
        QCOMPARE(changes.count(), 0);
        QCOMPARE(bindingRequests, baselineRequests + 1);
        // Phase 2: bob's deletion event lands while a newer snapshot that
        // still lists him is in flight.
        holdNext = true;
        f.controller.refreshChatHeaderMembers();
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(release), 5000);
        bound.removeAt(1); // the server dropped bob after the snapshot
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream_binding.deleted",
            QJsonObject{{"uuid", "61000000-0000-4000-8000-000000000002"},
                {"stream_uuid", streamId}, {"user_uuid", channelMemberB}}, 302)));
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2); // in-place event update
        QSignalSpy moreChanges(&f.controller, &NativeController::stateChanged); QVERIFY(moreChanges.isValid());
        release(); // the snapshot that still has bob arrives after the delete
        QTest::qWait(250);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 2);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 2);
        QCOMPARE(moreChanges.count(), 0);
        QCOMPARE(bindingRequests, baselineRequests + 2);
    }

    // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md): the sidebar stream
    // menu writes notification_mode through the same invoke endpoint, payload and
    // canonical-response rules as the chat-info switch, and it must also work for a
    // stream that is not the selected one: one request per level change, a per-stream
    // busy slot that rejects repeats while in flight, same-level no-ops, and the
    // streams()/topics() row projection of the mute state.
    void streamNotificationModeWritePathTargetsAnyStream_data()
    {
        QTest::addColumn<bool>("missingMode");
        QTest::newRow("stored-mode") << false;
        QTest::newRow("missing-mode") << true;
    }

    void streamNotificationModeWritePathTargetsAnyStream()
    {
        QFETCH(bool, missingMode);
        Fixture f;
        QJsonObject primary = creationChannel(streamId, "Muted board");
        primary["notification_mode"] = "muted";
        QJsonObject target = creationChannel(archiveId, "Silent channel");
        if (missingMode) target.remove("notification_mode");
        QJsonArray served{primary, target};
        f.streams = [&served](const Request &) { return Response{served}; };
        QJsonObject defaultTopic = creationTopic(topicId, streamId, "Native topic");
        defaultTopic["notification_mode"] = "default";
        QJsonObject followed = creationTopic(secondTopicId, streamId, "Followed topic");
        followed["is_default"] = false; followed["notification_mode"] = "follow";
        f.topics = [&defaultTopic, &followed](const Request &) {
            return Response{QJsonArray{defaultTopic, followed}};
        };
        int posts = 0; QString postPath; QJsonObject postBody;
        std::function<void()> release;
        const auto original = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/notifications/invoke")) {
                ++posts; postPath = request.target.path(); postBody = request.body;
                target["notification_mode"] = request.body.value("notification_mode");
                target["updated_at"] = "2026-01-02T11:00:00Z";
                served = QJsonArray{primary, target};
                Response response{target, 200, 0, {}};
                response.defer = [&](std::function<void()> send) { release = std::move(send); };
                return response;
            }
            return original(request);
        };
        auto &c = f.controller;
        c.login(f.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId);
        QTRY_COMPARE(c.streams().size(), 2);
        c.selectStream(streamId);
        QTRY_VERIFY(!c.topics().isEmpty());
        QTRY_VERIFY(!c.busy());
        const auto streamRow = [&](const QString &id) {
            for (const auto &value : c.streams())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };
        const auto topicRow = [&](const QString &id) {
            for (const auto &value : c.topics())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };
        // Row projections: the muted board carries its explicit follow, so it is not
        // fully muted, while its default topic is effectively muted and the followed
        // topic is not.
        QCOMPARE(streamRow(streamId).value("notificationMode").toString(), QStringLiteral("muted"));
        QVERIFY(!streamRow(streamId).value("fullyMuted").toBool());
        QCOMPARE(topicRow(topicId).value("notificationMode").toString(), QStringLiteral("default"));
        QVERIFY(topicRow(topicId).value("effectivelyMuted").toBool());
        QCOMPARE(topicRow(secondTopicId).value("notificationMode").toString(), QStringLiteral("follow"));
        QVERIFY(!topicRow(secondTopicId).value("effectivelyMuted").toBool());
        // Invalid inputs send nothing: unknown mode, unknown stream, same level.
        c.setStreamNotificationMode(archiveId, "whisper");
        c.setStreamNotificationMode(QStringLiteral("00000000-0000-4000-8000-0000000000ee"), QStringLiteral("muted"));
        c.setStreamNotificationMode(archiveId, missingMode ? QStringLiteral("mentions_only") : QStringLiteral("all_messages"));
        QTest::qWait(200);
        QCOMPARE(posts, 0);
        // The unselected target stream gets exactly one held request; the busy slot
        // is per stream and repeats are rejected while it is in flight.
        c.setStreamNotificationMode(archiveId, QStringLiteral("muted"));
        QTRY_COMPARE(posts, 1);
        QVERIFY(postPath.endsWith("/streams/" + archiveId + "/actions/notifications/invoke"));
        QCOMPARE(postBody.value("notification_mode").toString(), QStringLiteral("muted"));
        QVERIFY(c.streamNotificationBusy(archiveId));
        QVERIFY(!c.streamNotificationBusy(streamId));
        c.setStreamNotificationMode(archiveId, QStringLiteral("mentions_only"));
        QTest::qWait(200);
        QCOMPARE(posts, 1);
        release();
        QTRY_VERIFY(!c.streamNotificationBusy(archiveId));
        // The canonical response lands in the store: muted and, with no topics on
        // this stream, fully muted. The selected stream is untouched.
        QTRY_COMPARE(streamRow(archiveId).value("notificationMode").toString(), QStringLiteral("muted"));
        QVERIFY(streamRow(archiveId).value("fullyMuted").toBool());
        QCOMPARE(streamRow(streamId).value("notificationMode").toString(), QStringLiteral("muted"));
    }

    void streamNotificationRetiresInterruptedWrites_data()
    {
        QTest::addColumn<bool>("drawer");
        QTest::addColumn<bool>("committed");
        QTest::addColumn<bool>("logout");
        QTest::addColumn<bool>("lostResult");
        QTest::newRow("sidebar-committed") << false << true << false << false;
        QTest::newRow("drawer-uncommitted") << true << false << false << false;
        QTest::newRow("committed-with-error-response") << true << true << false << true;
        QTest::newRow("logout") << false << false << true << false;
    }

    void streamNotificationRetiresInterruptedWrites()
    {
        QFETCH(bool, drawer); QFETCH(bool, committed); QFETCH(bool, logout); QFETCH(bool, lostResult);
        auto stream = creationChannel(streamId);
        QList<std::function<void()>> replies;
        bool rotate = false;
        Fixture f;
        f.streams = [&](const Request &) { return Response{QJsonArray{stream}}; };
        f.authentication = [&](const Request &r) {
            if (rotate && r.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-renewed-notifications"},
                    {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(r);
        };
        const auto base = f.server.handler;
        f.server.handler = [&, base](const Request &r) -> Response {
            if (r.method != "POST" || !r.target.path().endsWith("/actions/notifications/invoke")) return base(r);
            auto result = stream;
            result["notification_mode"] = r.body.value("notification_mode");
            const bool apply = committed || !replies.isEmpty();
            const bool ok = apply && !(lostResult && replies.isEmpty());
            return {result, ok ? 200 : 503, 0, {}, [&, result, apply](std::function<void()> send) {
                replies.append([&, result, apply, send = std::move(send)] {
                    // The server can commit only now, after the client renewed.
                    if (apply) stream = result;
                    send();
                });
            }};
        };
        QVERIFY(f.start());
        auto &c = f.controller;
        if (drawer) c.setChannelInfoNotificationMode("muted");
        else c.setStreamNotificationMode(streamId, "muted");
        QTRY_COMPARE(replies.size(), 1);
        QVERIFY(c.channelInfoNotificationBusy());
        QVERIFY(c.streamNotificationBusy(streamId));
        if (logout) {
            c.logout();
            QVERIFY(!c.streamNotificationBusy(streamId));
            replies[0]();
            return;
        }
        QSignalSpy renewed(&f.session, &Session::authenticated);
        rotate = true;
        f.session.requestRefresh();
        QTRY_COMPARE(renewed.size(), 1);
        QTRY_VERIFY(!c.busy());
        QCOMPARE(c.channelInfoNotificationMode(), QString("all_messages"));
        // A changed token cannot acknowledge or cancel an already accepted
        // write on the server. Keep the original stream serialized until it
        // completes; a premature second POST could commit before the first.
        QVERIFY(c.channelInfoNotificationBusy());
        c.setStreamNotificationMode(streamId, "mentions_only");
        QCOMPARE(replies.size(), 1);
        replies[0]();
        QTRY_VERIFY(!c.channelInfoNotificationBusy());
        QTRY_COMPARE(c.channelInfoNotificationMode(), committed ? QString("muted") : QString("all_messages"));
        QVERIFY(!c.streamNotificationBusy(streamId));
        c.setChannelInfoNotificationMode("mentions_only");
        c.setStreamNotificationMode(streamId, "mentions_only");
        QTRY_COMPARE(replies.size(), 2);
        QVERIFY(c.channelInfoNotificationBusy());
        QCOMPARE(c.channelInfoNotificationMode(), committed ? QString("muted") : QString("all_messages"));
        replies[1]();
        QTRY_COMPARE(c.channelInfoNotificationMode(), QString("mentions_only"));
        QVERIFY(!c.streamNotificationBusy(streamId));
        QCOMPARE(replies.size(), 2);
        QCOMPARE(stream.value("notification_mode").toString(), QString("mentions_only"));
    }

    // Issue #438: the automatic member-page fetch follows the #434 scoping
    // discipline - only a selection edge whose chat header is actually on
    // screen may load member pages; mounting the header is not a selection
    // edge, the same owner+stream never refetches on its own, direct chats
    // never fetch, and a page that completes after its selection was
    // replaced stays silent (the completion guard that keeps a late page
    // from resurrecting counts of a conversation the user already left).
    void chatHeaderRetriesInitialMemberFailure_data()
    {
        QTest::addColumn<QString>("edge");
        for (const char *edge : {"visibility", "stream", "topic"})
            QTest::newRow(edge) << QString::fromLatin1(edge);
    }

    void chatHeaderRetriesInitialMemberFailure()
    {
        QFETCH(QString, edge);
        Fixture f;
        QVERIFY(f.start());
        f.controller.selectStream({});
        int reads = 0;
        f.bindings = [&](const Request &) -> Response {
            if (++reads == 1)
                return Response{QJsonArray{}, 500};
            auto binding = creationBinding(userId, 900);
            binding["stream_uuid"] = streamId;
            return Response{QJsonArray{binding}};
        };
        f.controller.setChatHeaderVisible(true);
        f.controller.selectStream(streamId);
        QTRY_COMPARE(reads, 1);
        QTRY_VERIFY(!f.controller.busy());
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        const auto reactivate = [&] {
            if (edge == "visibility") {
                f.controller.setChatHeaderVisible(false);
                f.controller.setChatHeaderVisible(true);
            } else if (edge == "stream") {
                f.controller.selectStream(streamId);
            } else {
                f.controller.selectTopic(topicId);
            }
        };
        reactivate();
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 1);
        QCOMPARE(reads, 2);
        reactivate();
        QTest::qWait(150);
        QCOMPARE(reads, 2);
    }

    void chatHeaderRetriesRetiredInitialMemberRequest()
    {
        Fixture f;
        QVERIFY(f.start());
        f.controller.selectStream({});
        int reads = 0;
        std::function<void()> release;
        f.bindings = [&](const Request &) -> Response {
            ++reads;
            auto binding = creationBinding(userId, 900);
            binding["stream_uuid"] = streamId;
            Response result{QJsonArray{binding}};
            if (reads == 1)
                result.defer = [&](std::function<void()> send) { release = std::move(send); };
            return result;
        };
        f.controller.setChatHeaderVisible(true);
        f.controller.selectStream(streamId);
        QTRY_VERIFY(release);
        f.api.setAccessToken("rotated-test-token");
        f.controller.selectStream(streamId);
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.chatHeaderMemberCount(), 1, 2000);
        QCOMPARE(reads, 2);
        release();
        QTest::qWait(150);
        QCOMPARE(reads, 2);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 1);
    }

    void chatHeaderCountersFetchFollowsVisibleSelectionEdges()
    {
        Fixture f;
        int bindingRequests = 0;
        bool holdNext = false;
        std::function<void()> release;
        const QString otherStream = QStringLiteral("8e0d0d00-0000-4000-8000-00000000000a");
        const QString directStreamUuid = QStringLiteral("8e0d0d00-0000-4000-8000-00000000000b");
        auto userRow = [](const QString &id, const QString &username, const QString &status) {
            return QJsonObject{{"uuid", id}, {"username", username}, {"first_name", username},
                {"email", username + "@example.invalid"}, {"status", status},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        QJsonArray directory{userRow(userId, "cassi", "active"),
            userRow(channelMemberB, "bob", "idle"), userRow(channelMemberA, "alice", "active")};
        f.users = [&directory](const Request &) { return Response{directory}; };
        auto binding = [](const QString &id, const QString &user, const QString &stream) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream},
                {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"},
                {"notification_mode", "all_messages"},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        f.streams = [&otherStream, &directStreamUuid](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}, {"role", "owner"}},
                QJsonObject{{"uuid", otherStream}, {"project_id", projectId},
                    {"name", "Solo channel"}, {"encryption", false}},
                QJsonObject{{"uuid", directStreamUuid}, {"project_id", projectId},
                    {"name", "Direct"}, {"private", true}, {"encryption", false}}}};
        };
        QJsonArray held; // the response snapshots `held` at request time
        f.bindings = [&held, &binding, &bindingRequests, &holdNext, &release, &otherStream](const Request &request) {
            ++bindingRequests;
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            Response response{stream == otherStream
                ? QJsonArray{binding("61000000-0000-4000-8000-00000000000b", channelMemberA, otherStream)}
                : held};
            if (holdNext) {
                holdNext = false;
                response.defer = [&release](std::function<void()> send) { release = std::move(send); };
            }
            return response;
        };
        QVERIFY(f.start({}, 3));
        // Headless selection never fetches: without an on-screen header the
        // projection must not produce member traffic at all.
        f.controller.selectStream(streamId);
        QTest::qWait(250);
        QCOMPARE(bindingRequests, 0);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        // Mounting a visible header over a restored selection loads its first page.
        held = QJsonArray{binding("61000000-0000-4000-8000-00000000000c", userId, streamId),
            binding("61000000-0000-4000-8000-00000000000d", channelMemberB, streamId)};
        f.controller.setChatHeaderVisible(true);
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 1);
        QCOMPARE(bindingRequests, 1);
        // A selection edge on the visible header loads exactly one page; the
        // page belongs to the stream that was selected, so bob (idle) is in
        // the member half but not the online half.
        f.controller.selectStream(otherStream);
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 1);
        QCOMPARE(bindingRequests, 2);
        f.controller.selectStream(streamId);
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 2);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 1);
        QCOMPARE(bindingRequests, 3);
        // Re-activating the same stream (a topic switch) is not an edge and
        // never re-reads the page (#434 budget).
        f.controller.selectStream(streamId);
        QTest::qWait(250);
        QCOMPARE(bindingRequests, 3);
        // Direct chats clear the projection without a request.
        f.controller.selectStream(directStreamUuid);
        QTest::qWait(250);
        QCOMPARE(bindingRequests, 3);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        // Issue #438 review (finding 4633233517): after the header has been
        // mounted, a selection edge skipped while it is off screen records
        // the obligation, and the rising visibility edge recovers it exactly
        // once - before the fix the caption stayed absent forever. A blink
        // with no skipped edge in between never re-reads.
        f.controller.setChatHeaderVisible(false);
        f.controller.selectStream(streamId);
        QTest::qWait(250);
        QCOMPARE(bindingRequests, 3);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        f.controller.setChatHeaderVisible(true);
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.chatHeaderMemberCount(), 2, 5000);
        QCOMPARE(bindingRequests, 4);
        f.controller.setChatHeaderVisible(false);
        f.controller.setChatHeaderVisible(true);
        QTest::qWait(250);
        QCOMPARE(bindingRequests, 4);
        QCOMPARE(f.controller.chatHeaderMemberCount(), 2);
        // A page that completes after its selection was replaced stays
        // silent: the late otherStream snapshot must not move the counts of
        // the conversation that is actually selected now.
        holdNext = true;
        f.controller.selectStream(otherStream);
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(release), 5000);
        QCOMPARE(bindingRequests, 5);
        f.controller.selectStream(directStreamUuid); // leaves the loading stream
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        release(); // the superseded page arrives after the selection moved on
        QTest::qWait(250);
        // The late page was fenced by the load ticket: no member set moved,
        // the direct selection keeps both counters at zero. (The transport's
        // own busy bookkeeping may still re-notify stateChanged; the pinned
        // contract is that the projection itself stays untouched.)
        QCOMPARE(f.controller.chatHeaderMemberCount(), 0);
        QCOMPARE(f.controller.chatHeaderOnlineCount(), 0);
    }
};

QTEST_MAIN(ControllerProfilesTest)
#include "native-controller-profiles-test.moc"
