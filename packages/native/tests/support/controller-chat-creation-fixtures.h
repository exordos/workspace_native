#pragma once

#include "native-test-harness.h"

inline QJsonObject creationTopic(const QString &id, const QString &stream, const QString &name)
{
    return {{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream}, {"user_uuid", userId},
        {"name", name}, {"is_done", false}, {"is_default", false}, {"unread_count", 0},
        {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}};
}

class TopicCreationFixture {
public:
    Fixture fixture;
    int posts = 0, confirmations = 0, catalogs = 0;
    int postStatus = 201, getStatus = 200, catalogStatus = 200, postDelay = 0, getDelay = 0, catalogDelay = 0;
    QJsonObject created;
    std::function<void(QJsonObject &)> alterPost, alterGet;
    bool includeCreated = true;
    explicit TopicCreationFixture()
    {
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/stream_topics/" && request.method == "POST") {
                ++posts;
                created = creationTopic(request.body.value("uuid").toString(),
                    request.body.value("stream_uuid").toString(), request.body.value("name").toString());
                auto result = created;
                if (alterPost) alterPost(result);
                return {result, postStatus, postDelay, {}};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/stream_topics/" + created.value("uuid").toString()) {
                ++confirmations;
                auto result = created;
                if (alterGet) alterGet(result);
                return {result, getStatus, getDelay, {}};
            }
            if (posts && path == "/api/workspace/v1/messenger/stream_topics/" && request.method == "GET") {
                ++catalogs;
                QJsonArray rows;
                const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                if (stream == streamId) rows.append(creationTopic(topicId, streamId, "Native topic"));
                if (includeCreated) rows.append(created);
                return {rows, catalogStatus, catalogDelay, {}};
            }
            return fallback(request);
        };
    }
};

inline const QString channelMemberA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
inline const QString channelMemberB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
inline const QString channelDefault = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

inline QJsonObject creationChannel(const QString &id, const QString &name = "Created channel")
{
    return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"owner", userId},
        {"name", name}, {"description", ""}, {"role", "owner"}, {"notification_mode", "all_messages"},
        {"source_name", "native"}, {"provider_uuid", QJsonValue::Null}, {"provider_identity", QJsonValue::Null},
        {"invite_only", false}, {"announce", false}, {"private", false}, {"is_archived", false},
        {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null}, {"direct_user_uuid", QJsonValue::Null},
        {"default_topic_uuid", channelDefault}, {"unread_count", 0}, {"active_unread_count", 0},
        {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

inline QJsonObject creationBinding(const QString &user, int number)
{
    return {{"uuid", historyMessage(number).value("uuid")}, {"project_id", projectId},
        {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"}, {"notification_mode", "all_messages"},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

class ChannelCreationFixture {
public:
    Fixture fixture;
    QJsonObject created, defaultTopic;
    QJsonArray bindings;
    QVector<QJsonObject> streamBodies, memberBodies;
    int posts = 0, gets = 0, topicGets = 0, streamGets = 0, memberPosts = 0, bindingGets = 0, usersGets = 0;
    int postStatus = 201, getStatus = 200, addStatus = 200, bindingStatus = 200, topicStatus = 200, catalogStatus = 200;
    int postDelay = 0, getDelay = 0, addDelay = 0, bindingDelay = 0, topicDelay = 0, catalogDelay = 0;
    bool includeCreated = true, includeDefault = true, applyMembers = true;
    std::function<void(QJsonObject &)> alterPost, alterGet;
    std::function<void(QJsonArray &)> alterBindings;
    ChannelCreationFixture()
    {
        fixture.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "self"}},
            QJsonObject{{"uuid", channelMemberA}, {"username", "alice"}, {"first_name", "Alice"},
                {"last_name", "Adams"}, {"email", "alice@example.invalid"}},
            QJsonObject{{"uuid", channelMemberB}, {"username", "bob"}, {"first_name", QJsonValue::Null},
                {"email", QJsonValue::Null}}}}; };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/users/") ++usersGets;
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "POST") {
                ++posts; streamBodies.append(request.body);
                created = creationChannel(request.body.value("uuid").toString(), request.body.value("name").toString());
                for (const auto *field : {"description", "invite_only", "announce"}) created[field] = request.body.value(field);
                defaultTopic = creationTopic(channelDefault, created.value("uuid").toString(), "General Topic");
                defaultTopic["is_default"] = true; defaultTopic["notification_mode"] = "default";
                defaultTopic["created_at"] = "2026-01-02T10:00:00Z";
                defaultTopic["updated_at"] = "2026-01-02T10:00:00Z";
                auto result = created; if (alterPost) alterPost(result);
                return {result, postStatus, postDelay};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/streams/" + created.value("uuid").toString()) {
                ++gets; auto result = created; if (alterGet) alterGet(result); return {result, getStatus, getDelay};
            }
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "GET") {
                ++streamGets; QJsonArray result{creationChannel(streamId, "Existing channel")};
                if (includeCreated && !created.isEmpty()) result.append(created);
                return {result, catalogStatus, catalogDelay};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == created.value("uuid").toString()) {
                ++topicGets; return {includeDefault ? QJsonArray{defaultTopic} : QJsonArray{}, topicStatus, topicDelay};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/streams/"
                + created.value("uuid").toString() + "/actions/add_users/invoke") {
                ++memberPosts; memberBodies.append(request.body);
                QJsonArray result; int n = 100;
                for (const auto &value : request.body.value("member").toArray()) {
                    auto row = creationBinding(value.toString(), n++); row["stream_uuid"] = created.value("uuid"); result.append(row);
                }
                if (applyMembers) bindings = result;
                if (alterBindings) alterBindings(result);
                return {result, addStatus, addDelay};
            }
            if (path == "/api/workspace/v1/messenger/stream_bindings/") {
                ++bindingGets; return {bindings, bindingStatus, bindingDelay};
            }
            return fallback(request);
        };
    }
};

// Issue #234: the encrypted fixture echoes the encrypted creation fields back
// exactly as the audited backend does: `encryption` and
// `current_encryption_key_uuid` mirror the accepted stream POST, its POST
// echo and every later canonical GET, unless a case corrupts them via
// alterEncryptedPost (POST echo only) or alterGet (canonical GET only).
class EncryptedChannelCreationFixture : public ChannelCreationFixture {
public:
    std::function<void(QJsonObject &)> alterEncryptedPost;
    EncryptedChannelCreationFixture()
    {
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            auto response = fallback(request);
            if (request.method == "POST" && request.target.path() == "/api/workspace/v1/messenger/streams/") {
                created["encryption"] = request.body.value("encryption");
                created["current_encryption_key_uuid"] = request.body.value("current_encryption_key_uuid");
                response.data = created;
                if (alterEncryptedPost) { auto row = response.data.toObject(); alterEncryptedPost(row); response.data = row; }
            }
            return response;
        };
    }
    // Attaches a durable chat key vault to the controller; every encrypted
    // creation commits its key here. The vault owner is an https identity
    // (canonicalOrigin requirement); the fixture server itself stays the
    // synthetic http mock.
    bool attachVault(NativeChatKeyVault &keys)
    {
        bool opened = false;
        NativeChatKeyVault::Failure failure = NativeChatKeyVault::Failure::Unavailable;
        keys.open(QUrl("https://channel-creation.invalid"), projectId, userId, "synthetic device",
            [&](NativeChatKeyVault::Failure value) { failure = value; opened = value == NativeChatKeyVault::Failure::None; });
        const bool ok = QTest::qWaitFor([&opened] { return opened; }, 2000);
        if (!ok) qWarning("vault open failed with code %d", int(failure));
        if (ok) fixture.controller.useChatKeys(&keys);
        return ok;
    }
};

inline const QString directId = "8c471e8b-a67b-5167-aeb7-b5ea70d13133";

inline QString expectedDirectId(const QString &peer, const QString &viewer = userId, const QString &project = projectId)
{
    QStringList pair{peer.toLower(), viewer.toLower()}; pair.sort();
    return QUuid::createUuidV5(QUuid("26f42131-4503-5f59-8aa6-32b20f551751"),
        (project.toLower() + ":" + pair.join(':')).toUtf8()).toString(QUuid::WithoutBraces);
}

inline QJsonObject creationDirect(const QString &peer = channelMemberA, const QString &id = {})
{
    auto row = creationChannel(id.isEmpty() ? expectedDirectId(peer) : id, "Direct");
    row["description"] = "Private workspace"; row["private"] = true; row["direct_user_uuid"] = peer;
    return row;
}

class DirectChatFixture {
public:
    Fixture fixture;
    QJsonObject created, defaultTopic;
    int posts = 0, gets = 0, topicGets = 0, streamGets = 0, usersGets = 0;
    int postStatus = 201, lookupStatus = 0, topicStatus = 200, catalogStatus = 200;
    int postDelay = 0, getDelay = 0, topicDelay = 0, catalogDelay = 0;
    bool applyPost = true, includeDefault = true, includeCreated = true;
    QVector<QJsonObject> bodies;
    std::function<void(QJsonObject &)> alterPost, alterGet;
    DirectChatFixture()
    {
        fixture.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "self"}},
            QJsonObject{{"uuid", channelMemberA}, {"username", "alice"}, {"first_name", "Alice"},
                {"last_name", "Adams"}, {"email", "alice@example.invalid"}},
            QJsonObject{{"uuid", channelMemberB}, {"username", "bob"}}}}; };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/users/") ++usersGets;
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "POST") {
                ++posts; bodies.append(request.body);
                auto row = creationDirect(request.body.value("direct_user_uuid").toString());
                if (applyPost) seed(row);
                if (alterPost) alterPost(row);
                return {row, postStatus, postDelay};
            }
            if (request.method == "GET" && (path == "/api/workspace/v1/messenger/streams/" + directId
                || path == "/api/workspace/v1/messenger/streams/" + expectedDirectId(channelMemberB))) {
                ++gets; auto row = created; if (alterGet) alterGet(row);
                return {row, lookupStatus ? lookupStatus : created.isEmpty() ? 404 : 200, getDelay};
            }
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "GET") {
                ++streamGets; QJsonArray rows{creationChannel(streamId, "Existing channel")};
                if (includeCreated && !created.isEmpty() && (gets || posts)) rows.append(created);
                return {rows, catalogStatus, catalogDelay};
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == created.value("uuid").toString()) {
                ++topicGets; return {includeDefault ? QJsonArray{defaultTopic} : QJsonArray{}, topicStatus, topicDelay};
            }
            return fallback(request);
        };
    }
    void seed(QJsonObject row = creationDirect())
    {
        created = row;
        defaultTopic = creationTopic(channelDefault, row.value("uuid").toString(), "General Topic");
        defaultTopic["is_default"] = true; defaultTopic["notification_mode"] = "default";
        defaultTopic["created_at"] = "2026-01-02T10:00:00Z";
        defaultTopic["updated_at"] = "2026-01-02T10:00:00Z";
    }
};
