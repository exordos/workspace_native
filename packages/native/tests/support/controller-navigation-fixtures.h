#pragma once

#include "native-test-harness.h"
#include "controller-chat-creation-fixtures.h"

inline const QString archiveId = "90909090-9090-4090-8090-909090909090";
inline const QString otherArchiveId = "91919191-9191-4191-8191-919191919191";
class ArchiveFixture {
public:
    Fixture fixture;
    QJsonObject archived = creationChannel(archiveId, "Archived channel"), other = creationChannel(otherArchiveId, "Other archived channel");
    int posts = 0, gets = 0, postStatus = 200, getStatus = 200, postDelay = 0, getDelay = 0, catalogDelay = 0;
    bool applyPost = true, includeOther = false, closed = false;
    std::function<Response(const Request &)> history;
    std::function<void(QJsonObject &)> postDto, getDto;
    QMap<QString,QJsonObject> additional;
    ArchiveFixture() {
        archived["is_archived"] = true; other["is_archived"] = true;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path(), prefix = QString("/api/workspace/v1/messenger/streams/");
            const auto target=path.mid(prefix.size()).section('/',0,0);
            const bool known=target==archiveId || target==otherArchiveId || additional.contains(target);
            if (path.startsWith(prefix) && known && path.endsWith("/actions/unarchive/invoke")) {
                ++posts; auto &row = target==archiveId ? archived : target==otherArchiveId ? other : additional[target];
                if (applyPost) { row["is_archived"] = false; row["updated_at"] = "2026-01-02T10:01:00Z"; }
                auto result = row; if (postDto) postDto(result); return {result, postStatus, postDelay, {}};
            }
            if (request.method == "GET" && known && path == prefix+target) {
                ++gets; auto row = target==archiveId ? archived : target==otherArchiveId ? other : additional.value(target);
                if (getDto) getDto(row); return {row, getStatus, getDelay, {}};
            }
            if (path == prefix && request.method == "GET") {
                QJsonArray rows{creationChannel(streamId, "Active channel"), archived}; if (includeOther) rows.append(other); for(const auto &row:additional) rows.append(row); return {rows,200,catalogDelay,{}};
            }
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (path.endsWith("/stream_topics/") && (stream == archiveId || stream == otherArchiveId)) {
                auto topic = creationTopic(topicId, stream, "Archived topic"); topic["is_done"] = closed; return {QJsonArray{topic}};
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/" + topicId && fixture.controller.selectedStream() == archiveId) {
                auto topic = creationTopic(topicId, archiveId, "Archived topic"); topic["is_done"] = closed; return {topic};
            }
            if (path.endsWith("/messages/") && (stream == archiveId || stream == otherArchiveId)) {
                if (history) return history(request);
                auto row = message(); row["stream_uuid"] = stream; return {QJsonArray{row}};
            }
            return fallback(request);
        };
    }
    bool start() { return fixture.start(); }
};

inline QVariantMap archivedChat(const NativeController &controller, const QString &id = archiveId)
{
    for (const auto &row : controller.archivedChats()) if (row.toMap().value("id").toString() == id) return row.toMap();
    return {};
}

inline const QString openingStreamId = "abababab-abab-4bab-8bab-abababababab";
inline const QString openingRequestedId = "cdcdcdcd-cdcd-4dcd-8dcd-cdcdcdcdcdcd";
inline const QString openingCanonicalId = "efefefef-efef-4fef-8fef-efefefefefef";

class TopicOpeningFixture {
public:
    Fixture fixture;
    QJsonObject resolved = creationTopic(openingRequestedId, openingStreamId, "Canonical topic");
    QJsonObject destination = creationChannel(openingStreamId, "Destination channel");
    QJsonArray history;
    bool includeDestination = false, canonicalSeen = false, recordRequests = false;
    int lookupStatus = 200, lookupDelay = 0, detailStatus = 200, detailDelay = 0;
    int topicsStatus = 200, topicsDelay = 0;
    int lookups = 0, details = 0, catalogs = 0, users = 0, mutations = 0;
    int aggregateHistory = 0, topicHistory = 0, targetTopicLists = 0;
    QVector<QUrl> historyTargets;
    QStringList lookupTargets, unexpectedRequests;
    QMap<QString, Response> additionalLookups;
    bool omitResolvedFromList = false;
    std::function<void(QJsonObject &)> alterLookup;

    TopicOpeningFixture()
    {
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (recordRequests) {
                const bool allowedGet = request.method == "GET"
                    && (path == "/api/workspace/v1/users/"
                        || path == "/api/workspace/v1/messenger/streams/"
                        || path == "/api/workspace/v1/messenger/streams/" + openingStreamId
                        || path == "/api/workspace/v1/messenger/stream_topics/"
                        || path == "/api/workspace/v1/messenger/stream_topics/" + openingRequestedId
                        || additionalLookups.contains(path)
                        || path == "/api/workspace/v1/messenger/messages/");
                if (!allowedGet) unexpectedRequests.append(QString::fromUtf8(request.method)
                    + " " + request.target.toString(QUrl::FullyEncoded));
            }
            if (request.method != "GET") {
                if (path.startsWith("/api/workspace/v1/messenger/")) ++mutations;
                return fallback(request);
            }
            if (additionalLookups.contains(path)) {
                ++lookups;
                lookupTargets.append(path);
                canonicalSeen = true;
                return additionalLookups.value(path);
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/" + openingRequestedId) {
                ++lookups;
                lookupTargets.append(path);
                canonicalSeen = true;
                auto row = resolved;
                if (alterLookup) alterLookup(row);
                return {row, lookupStatus, lookupDelay, {}};
            }
            if (path == "/api/workspace/v1/messenger/streams/" + openingStreamId) {
                ++details;
                return {destination, detailStatus, detailDelay, {}};
            }
            if (path == "/api/workspace/v1/messenger/streams/") {
                ++catalogs;
                QJsonArray rows{creationChannel(streamId, "Origin channel")};
                if (includeDestination) rows.append(destination);
                return {rows};
            }
            if (path == "/api/workspace/v1/users/") ++users;
            if (path == "/api/workspace/v1/messenger/stream_topics/") {
                const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                QJsonArray rows;
                if (stream == streamId) rows = {creationTopic(topicId, streamId, "Origin topic"),
                    creationTopic(secondTopicId, streamId, "Other known topic")};
                if (canonicalSeen && stream == resolved.value("stream_uuid").toString()) {
                    ++targetTopicLists;
                    for (qsizetype i = rows.size(); i > 0; --i)
                        if (rows.at(i - 1).toObject().value("uuid") == resolved.value("uuid")) rows.removeAt(i - 1);
                    if (!omitResolvedFromList) rows.append(resolved);
                    return {rows, topicsStatus, topicsDelay, {}};
                }
                return {rows};
            }
            if (path == "/api/workspace/v1/messenger/messages/") {
                const QUrlQuery query(request.target);
                if (query.queryItemValue("mentioned") == "true") return fallback(request);
                historyTargets.append(request.target);
                if (query.hasQueryItem("topic_uuid")) ++topicHistory; else ++aggregateHistory;
                if (canonicalSeen && query.queryItemValue("stream_uuid") == resolved.value("stream_uuid").toString()
                    && query.queryItemValue("topic_uuid") == resolved.value("uuid").toString()) return {history};
                return {QJsonArray{}};
            }
            return fallback(request);
        };
    }

    bool start()
    {
        auto &controller = fixture.controller;
        controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([&] { return controller.projects().size() == 1; }, 2000)) return false;
        controller.openProject(projectId);
        const int expected = includeDestination && !destination.value("is_archived").toBool() ? 2 : 1;
        if (!QTest::qWaitFor([&] { return controller.streams().size() == expected; }, 2000)) return false;
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([&] { return controller.topics().size() == 2; }, 2000)) return false;
        controller.selectTopic(topicId);
        if (!QTest::qWaitFor([&] { return !controller.busy(); }, 2000)) return false;
        if (!QTest::qWaitFor([&] { return controller.channelMembersVerified(); }, 2000)) return false;
        lookups = details = catalogs = users = mutations = aggregateHistory = topicHistory = targetTopicLists = 0;
        historyTargets.clear();
        lookupTargets.clear();
        unexpectedRequests.clear();
        recordRequests = true;
        return true;
    }

    QPointer<QNetworkReply> openingReply(bool detail = false) const
    {
        const auto path = detail ? "/api/workspace/v1/messenger/streams/" + openingStreamId
                                 : "/api/workspace/v1/messenger/stream_topics/" + openingRequestedId;
        for (auto *reply : fixture.api.findChildren<QNetworkReply *>())
            if (reply->url().path() == path) return reply;
        return {};
    }
};
