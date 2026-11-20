#pragma once

#include "native-test-harness.h"
#include "controller-identity-fixtures.h"

// Issue #244 review 4577108882: the durable dispatch marker is the gate in
// front of the key_announced POST. This seam lets a test fail exactly the
// marker write (nothing else) and prove the reissue aborts before dispatch.
struct FailingRotationMarkerVault final : public NativeChatKeyVault {
    using NativeChatKeyVault::NativeChatKeyVault;
    bool failDispatchMarker = false;
    Failure markRotationStageDispatched(const Lease &lease, const Address &address) override
    {
        if (failDispatchMarker) return Failure::Unavailable;
        return NativeChatKeyVault::markRotationStageDispatched(lease, address);
    }
};

// Issue #244 (docs/NATIVE_DESKTOP_E2EE_KEY_MANAGEMENT.md): catalog-backed
// encrypted stream + durable key vault, serving the key-screen contract:
// local-key inventory, reissue and delete-local-key controller actions.
struct KeyManagementFixture {
    static inline const QString KeyCurrent = "11111111-1111-4111-9111-111111111111";
    static inline const QString KeyOld = "22222222-2222-4222-9222-222222222222";
    static inline const QString KeyOrphan = "33333333-3333-4333-9333-333333333333";
    static inline const QString SecondMember = "99999999-9999-4999-8999-999999999999";
    MockServer server;
    LoopbackKeyTls tls{server.serverPort()};
    ApiClient api{nullptr, true};
    Session session{&api};
    NativeController controller{&api, &session};
    QTemporaryDir directory;
    ControllerMemorySecrets secrets;
    FailingRotationMarkerVault vault;
    QString viewerRole = "owner";
    int messagePostStatus = 201;
    bool deferAnnouncements = false;
    int announcementPosts = 0;
    QJsonArray announcementBodies;
    std::function<void()> releaseAnnouncement;
    // R2 review 4576750804: the saved-generation re-request needs the same
    // deterministic key_request echo the controller binds after a POST.
    int requestPosts = 0;
    QJsonArray requestBodies;
    // R2 review 4576750798: a restart resolves a lost announcement response by
    // GET-ing the announcement message under its uuid; the server only has the
    // row when the POST actually landed (message uuid -> key uuid).
    QHash<QString, QString> serverAnnouncedMessages;
    // R3 review 4577108882: the announcement-GET rechecks can be held and
    // released one at a time, so a test can change the durable stage record
    // in the window between a scan's snapshot and the arrival of its answer.
    int messageCheckGets = 0;
    bool deferMessageChecks = false;
    std::function<void()> releaseMessageCheck;
    explicit KeyManagementFixture() : vault(directory.path(), &secrets)
    {
        server.handler = [this](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/")
                return {QJsonArray{streamRow()}, 200};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return {streamRow(), 200};
            if (path == "/api/workspace/v1/messenger/stream_topics/")
                return {QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                             {"stream_uuid", streamId}, {"name", "History"}, {"is_default", false}},
                    QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId},
                        {"stream_uuid", streamId}, {"name", "Default"}, {"is_default", true}}}, 200};
            if (path == "/api/workspace/v1/messenger/stream_bindings/")
                return {QJsonArray{QJsonObject{{"uuid", "40000000-0000-4000-8000-000000000001"},
                             {"project_id", projectId}, {"stream_uuid", streamId},
                             {"user_uuid", userId}, {"role", viewerRole}},
                    QJsonObject{{"uuid", "40000000-0000-4000-8000-000000000002"},
                        {"project_id", projectId}, {"stream_uuid", streamId},
                        {"user_uuid", SecondMember}, {"role", "member"}}}, 200};
            if (path == "/api/workspace/v1/users/")
                return {QJsonArray{}};
            if (path == "/api/workspace/v1/messenger/messages/" && request.method == "GET")
                return {QJsonArray{announcement("50000000-0000-4000-8000-000000000001", KeyCurrent,
                             "2026-01-02T10:00:00.000Z"),
                    announcement("50000000-0000-4000-8000-000000000002", KeyCurrent,
                        "2026-01-01T08:00:00.000Z"),
                    announcement("50000000-0000-4000-8000-000000000003", KeyOld,
                        "2025-12-01T00:00:00.000Z")}, 200};
            if (path == "/api/workspace/v1/messenger/messages/" && request.method == "POST") {
                const auto payload = request.body.value("payload").toObject();
                if (payload.value("kind").toString() == "key_announced") {
                    ++announcementPosts;
                    announcementBodies.append(request.body);
                    auto saved = message({}, "60000000-0000-4000-8000-000000000001", secondTopicId);
                    saved["payload"] = payload;
                    Response response{saved, messagePostStatus};
                    if (deferAnnouncements)
                        response.defer = [this](std::function<void()> send) { releaseAnnouncement = send; };
                    return response;
                }
                if (payload.value("kind").toString() == "key_request") {
                    ++requestPosts;
                    requestBodies.append(request.body);
                    // The controller binds the request by its deterministic placement
                    // (V5 of the topic over the local request id), so the accepted
                    // row must carry exactly that uuid.
                    const auto topic = request.body.value("topic_uuid").toString();
                    const auto placement = QUuid::createUuidV5(QUuid(topic),
                        request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                    auto saved = message({}, placement, topic);
                    saved["payload"] = payload;
                    return {saved, messagePostStatus};
                }
                return {message({}, "60000000-0000-4000-8000-000000000009", topicId), 201};
            }
            if (request.method == "GET" && path.startsWith("/api/workspace/v1/messenger/messages/")) {
                ++messageCheckGets;
                const auto uuid = path.mid(QString::fromLatin1("/api/workspace/v1/messenger/messages/").size()).toLower();
                const auto landed = serverAnnouncedMessages.constFind(uuid);
                if (landed == serverAnnouncedMessages.constEnd()) {
                    Response response{QJsonObject{}, 404};
                    if (deferMessageChecks)
                        response.defer = [this](std::function<void()> send) { releaseMessageCheck = send; };
                    return response;
                }
                auto row = message({}, uuid, secondTopicId);
                row["payload"] = QJsonObject{{"kind", "key_announced"}, {"key_uuid", *landed}};
                return {row, 200};
            }
            return sessionResponse(request);
        };
    }
    QJsonObject streamRow() const
    {
        return {{"uuid", streamId}, {"project_id", projectId}, {"name", "Key management fixture"},
            {"description", "Synthetic key management stream"}, {"user_uuid", userId}, {"owner", userId},
            {"role", viewerRole}, {"source_name", "native"}, {"provider_uuid", QJsonValue::Null},
            {"provider_identity", QJsonValue::Null}, {"encryption", true},
            {"current_encryption_key_uuid", KeyCurrent}, {"default_topic_uuid", secondTopicId},
            {"private", false}, {"direct_user_uuid", QJsonValue::Null}, {"invite_only", false},
            {"announce", false}, {"is_archived", false}, {"unread_count", 0},
            {"active_unread_count", 0}, {"passive_unread_count", 0},
            {"last_message_uuid", QJsonValue::Null}, {"notification_mode", "all_messages"},
            {"created_at", "2026-01-01T00:00:00.000Z"}, {"updated_at", "2026-01-02T12:00:00.000Z"}};
    }
    static QJsonObject announcement(const QString &uuid, const QString &key, const QString &createdAt)
    {
        auto row = message({}, uuid, topicId);
        row["created_at"] = createdAt;
        row["payload"] = QJsonObject{{"kind", "key_announced"}, {"key_uuid", key}};
        return row;
    }
    bool start(bool attachVault = true)
    {
        controller.login(tls.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([this] { return controller.projects().size() == 1; }, 2000)) return false;
        controller.openProject(projectId);
        if (!QTest::qWaitFor([this] { return controller.streams().size() == 1; }, 2000)) return false;
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([this] { return !controller.topics().isEmpty(); }, 2000)) return false;
        controller.selectTopic(topicId);
        if (!QTest::qWaitFor([this] { return !controller.busy() && controller.messages().size() == 3; }, 2000)) return false;
        if (!attachVault) return true;
        attachVaultToController();
        return true;
    }
    void attachVaultToController()
    {
        NativeChatKeyVault::Failure openFailure = NativeChatKeyVault::Failure::Unavailable;
        vault.open(tls.origin(), projectId, userId, "Synthetic key device",
            [&openFailure](NativeChatKeyVault::Failure failure) { openFailure = failure; });
        QTest::qWaitFor([&openFailure] { return openFailure != NativeChatKeyVault::Failure::Unavailable; }, 2000);
        QVERIFY2(openFailure == NativeChatKeyVault::Failure::None,
            QByteArray("vault open failed: ") + QByteArray::number(static_cast<int>(openFailure)));
        controller.useChatKeys(&vault);
    }
    void seedKey(const QString &key)
    {
        QCOMPARE(vault.put(vault.lease(), {streamId, key}, QByteArray(32, char(7)), 0),
            NativeChatKeyVault::Failure::None);
    }
    QVariantList inventoryRows() const { return controller.chatKeyInventory().value("keys").toList(); }
    QStringList inventoryUuids() const
    {
        QStringList result;
        for (const auto &row : inventoryRows()) result.append(row.toMap().value("keyUuid").toString());
        return result;
    }
};

// R2 review 4576750798: restart simulation. The live controller/vault pair is
// torn down (the announcement response never returns), and a fresh client,
// session, controller and vault come up over the same durable vault directory
// and the same mock server. The old vault is closed first so the single-writer
// vault lock transfers, exactly like a process restart would release it.
struct KeyManagementRestart {
    KeyManagementFixture &fixture;
    ApiClient api{nullptr, true};
    Session session{&api};
    NativeController controller{&api, &session};
    NativeChatKeyVault vault;
    explicit KeyManagementRestart(KeyManagementFixture &owner)
        : fixture(owner), vault(owner.directory.path(), &owner.secrets) {}
    bool start()
    {
        fixture.vault.close();
        controller.login(fixture.tls.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([this] { return controller.projects().size() == 1; }, 2000)) return false;
        controller.openProject(projectId);
        if (!QTest::qWaitFor([this] { return controller.streams().size() == 1; }, 2000)) return false;
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([this] { return !controller.topics().isEmpty(); }, 2000)) return false;
        controller.selectTopic(topicId);
        if (!QTest::qWaitFor([this] { return !controller.busy() && controller.messages().size() == 3; }, 2000)) return false;
        NativeChatKeyVault::Failure openFailure = NativeChatKeyVault::Failure::Unavailable;
        vault.open(fixture.tls.origin(), projectId, userId, "Synthetic key device",
            [&openFailure](NativeChatKeyVault::Failure failure) { openFailure = failure; });
        if (!QTest::qWaitFor([&openFailure] { return openFailure != NativeChatKeyVault::Failure::Unavailable; }, 2000))
            return false;
        if (openFailure != NativeChatKeyVault::Failure::None) return false;
        controller.useChatKeys(&vault);
        return true;
    }
    QVariantList inventoryRows() const { return controller.chatKeyInventory().value("keys").toList(); }
    QStringList inventoryUuids() const
    {
        QStringList result;
        for (const auto &row : inventoryRows()) result.append(row.toMap().value("keyUuid").toString());
        return result;
    }
};
