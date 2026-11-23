#include "support/native-test-harness.h"
#include "support/controller-chat-creation-fixtures.h"
#include "support/controller-navigation-fixtures.h"
#include "support/controller-media-fixtures.h"
#include "support/controller-folder-fixtures.h"
#include "support/controller-mention-fixtures.h"
#include "support/controller-poll-fixtures.h"

class ControllerSidebarTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void sidebarChatActionsOwnTheirTarget_data()
    {
        QTest::addColumn<QString>("action"); QTest::addColumn<QString>("scenario");
        for (const auto &action : {"rename", "read", "archive"})
            for (const auto &scenario : {"success", "refusal", "foreign", "newer", "acl", "logout", "deleted"})
                QTest::newRow(qPrintable(QString(action) + "-" + scenario)) << QString(action) << QString(scenario);
    }
    void sidebarChatActionsOwnTheirTarget()
    {
        QFETCH(QString, action); QFETCH(QString, scenario);
        ArchiveFixture test; auto &c = test.fixture.controller;
        test.archived["is_archived"] = false; test.archived["unread_count"] = 2; test.archived["active_unread_count"] = 2;
        const auto original = test.fixture.server.handler;
        int mutations = 0;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if ((path.endsWith("/streams/"+archiveId) && request.method == "PUT")
                || path.endsWith("/streams/"+archiveId+"/actions/read/invoke")
                || path.endsWith("/streams/"+archiveId+"/actions/archive/invoke")) {
                ++mutations;
                auto response = test.archived;
                response["updated_at"] = "2026-01-02T10:01:00Z";
                if (action == "rename") { if (request.body.value("name").toString() != "Renamed team") return {QJsonObject{}, 400}; response["name"] = "Renamed team"; }
                else if (action == "read") { response["unread_count"] = 0; response["active_unread_count"] = 0; }
                else response["is_archived"] = true;
                if (scenario == "foreign") response["user_uuid"] = otherArchiveId;
                if (scenario == "success") test.archived = response;
                return {response, scenario == "refusal" ? 403 : 200, 120, {}};
            }
            return original(request);
        };
        // The target is deliberately not the selected channel.
        c.login(test.fixture.server.origin().toString(), "user", "password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId); QTRY_COMPARE(c.streams().size(), 2);
        c.selectStream(streamId); QTRY_VERIFY(!c.busy());
        const auto target = [&] { for (const auto &v : c.streams()) if (v.toMap().value("id") == archiveId) return v.toMap(); return QVariantMap{}; };
        const auto submit = [&] { if (action == "rename") c.renameChat(archiveId, "  Renamed team  "); else if (action == "read") c.markChatRead(archiveId); else c.archiveChat(archiveId); };
        QSignalSpy markedRead(&c, &NativeController::conversationMarkedRead);
        submit(); submit(); QTRY_COMPARE(mutations, 1);
        QVERIFY(c.chatActionState(archiveId).value("pending").toBool());
        if (scenario == "logout") c.logout();
        else if (scenario == "deleted") QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.deleted", QJsonObject{{"uuid", archiveId}})));
        else if (scenario == "acl" || scenario == "newer") {
            auto event = test.archived; event["updated_at"] = "2026-01-02T10:02:00Z";
            event["name"] = "Live name"; event["unread_count"] = 7; event["active_unread_count"] = 7;
            if (scenario == "acl") event["role"] = "guest";
            QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", event)));
            if (scenario == "acl") { event["role"] = "owner"; QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", event))); }
        }
        QTest::qWait(180);
        QCOMPARE(mutations, 1);
        const bool acceptedRead = action == "read" && (scenario == "success" || scenario == "newer");
        QCOMPARE(markedRead.size(), acceptedRead ? 1 : 0);
        if (acceptedRead) {
            QCOMPARE(markedRead.first().at(0).toString(), archiveId);
            QVERIFY(markedRead.first().at(1).toString().isEmpty());
        }
        if (scenario == "logout") { QVERIFY(!c.authenticated()); QVERIFY(c.streams().isEmpty()); return; }
        if (scenario == "deleted") { QVERIFY(target().isEmpty()); return; }
        QCOMPARE(c.selectedStream(), streamId);
        if (scenario == "success") {
            if (action == "archive") { QVERIFY(target().isEmpty()); QCOMPARE(c.archivedChats().size(), 1); c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QVERIFY(!target().isEmpty()); }
            else if (action == "rename") { QCOMPARE(target().value("name").toString(), QString("Renamed team")); QCOMPARE(target().value("unread").toInt(), 2); }
            else { QCOMPARE(target().value("unread").toInt(), 0); QCOMPARE(target().value("name").toString(), QString("Archived channel")); }
        } else if (scenario == "newer" || scenario == "acl") {
            QCOMPARE(target().value("name").toString(), QString("Live name")); QCOMPARE(target().value("unread").toInt(), 7);
        } else {
            QVERIFY(!c.chatActionState(archiveId).value("error").toString().isEmpty());
            QCOMPARE(target().value("name").toString(), QString("Archived channel")); QCOMPARE(target().value("unread").toInt(), 2);
        }
    }
    void sidebarChatActionCapacityMatchesAvailability_data()
    {
        QTest::addColumn<bool>("read"); QTest::addColumn<bool>("stale");
        for (const bool read : {false, true}) for (const bool stale : {false, true})
            QTest::addRow("%s-%s", read ? "read" : "rename", stale ? "stale-slot" : "completed-slot") << read << stale;
    }
    void sidebarChatActionCapacityMatchesAvailability()
    {
        QFETCH(bool, read); QFETCH(bool, stale);
        Fixture f; QJsonArray streams; QStringList ids;
        QMap<QString, QJsonObject> rows;
        for (int i = 0; i < 33; ++i) {
            const auto id = QString("aaaaaaaa-aaaa-4aaa-8aaa-%1").arg(i + 1, 12, 16, QLatin1Char('0'));
            auto row = creationChannel(id, QString("Capacity %1").arg(i));
            row["unread_count"] = 1; row["active_unread_count"] = 1;
            rows.insert(id, row); ids.append(id); streams.append(row);
        }
        f.streams = [&](const Request &) { return Response{streams}; };
        QVector<QPair<QString, std::function<void()>>> held;
        QStringList submitted; bool drain = false;
        const auto original = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.contains("/streams/") && (request.method == "PUT" || path.endsWith("/actions/read/invoke"))) {
                const auto id = path.section("/streams/", 1).section('/', 0, 0);
                submitted.append(id);
                auto row = rows.value(id);
                if (read) { row["unread_count"] = 0; row["active_unread_count"] = 0; }
                else row["name"] = request.body.value("name");
                Response response{row};
                if (!drain) response.defer = [&, id](std::function<void()> send) { held.append({id, std::move(send)}); };
                return response;
            }
            return original(request);
        };
        auto &c = f.controller;
        c.login(f.server.origin().toString(), "user", "password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId); QTRY_COMPARE(c.streams().size(), 33);
        const auto submit = [&](const QString &id) { if (read) c.markChatRead(id); else c.renameChat(id, "Changed"); };
        for (int i = 0; i < 32; ++i) {
            QVERIFY(c.chatActionState(ids[i]).value("available").toBool());
            submit(ids[i]); QVERIFY(c.chatActionState(ids[i]).value("pending").toBool());
        }
        QTRY_VERIFY(!held.isEmpty());
        const auto target = ids.last();
        const bool advertisedUnavailable = !c.chatActionState(target).value("available").toBool();
        QVERIFY(c.chatActionState(target).value("canArchive").toBool());
        submit(target);
        QVERIFY(!c.chatActionState(target).value("pending").toBool());
        QVERIFY(!submitted.contains(target));
        QSignalSpy changed(&c, &NativeController::streamsChanged);
        const auto released = held.first().first;
        if (stale) {
            auto row = rows.value(released); row["role"] = "guest";
            row["updated_at"] = "2026-01-02T10:02:00Z";
            QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", row)));
        } else held.first().second();
        QTRY_VERIFY(!c.chatActionState(released).value("pending").toBool());
        QVERIFY(!changed.isEmpty());
        QVERIFY(c.chatActionState(target).value("available").toBool());
        submit(target); QVERIFY(c.chatActionState(target).value("pending").toBool());
        // Qt may queue requests behind its per-host transport limit. Releasing
        // the accepted sockets lets every admitted operation reach the server.
        drain = true;
        const auto releases = held; held.clear();
        for (const auto &entry : releases) if (stale || entry.first != released) entry.second();
        QTRY_COMPARE(submitted.size(), 33);
        QTRY_VERIFY(!c.chatActionState(target).value("pending").toBool());
        QCOMPARE(submitted.count(target), 1);
        QVERIFY(c.chatActionState(target).value("error").toString().isEmpty());
        QVERIFY2(advertisedUnavailable, "The 33rd read/rename was advertised as available although admission silently rejected it");
    }
    void sidebarNewActionReplacesPreviousFailure()
    {
        Fixture f; auto stream = creationChannel(streamId, "Original");
        f.streams = [&](const Request &) { return Response{QJsonArray{stream}}; };
        const auto original = f.server.handler;
        f.server.handler = [&](const Request &r) -> Response {
            if (r.target.path().endsWith("/actions/archive/invoke")) return {QJsonObject{}, 403};
            if (r.method == "PUT" && r.target.path().endsWith("/streams/"+streamId)) { stream["name"] = r.body.value("name"); return {stream}; }
            return original(r);
        };
        QVERIFY(f.start()); auto &c = f.controller;
        c.archiveChat(streamId); QTRY_VERIFY(!c.chatActionState(streamId).value("pending").toBool());
        QVERIFY(!c.chatActionState(streamId).value("error").toString().isEmpty());
        c.renameChat(streamId, "Renamed"); QTRY_COMPARE(c.streams().first().toMap().value("name").toString(), QString("Renamed"));
        QVERIFY2(c.chatActionState(streamId).value("error").toString().isEmpty(), "A successful new action must not inherit an older archive failure");
    }
    void sidebarReadPreservesMessagesArrivingDuringPost()
    {
        Fixture f; auto stream = creationChannel(streamId, "Original");
        stream["unread_count"] = 1; stream["active_unread_count"] = 1;
        auto oldMessage = message(); oldMessage["read"] = false;
        f.streams = [&](const Request &) { return Response{QJsonArray{stream}}; };
        f.messages = [&](const Request &) { return Response{QJsonArray{oldMessage}}; };
        int posts = 0; const auto original = f.server.handler;
        f.server.handler = [&](const Request &r) -> Response {
            if (r.target.path().endsWith("/actions/read/invoke")) {
                ++posts; auto response = stream; response["unread_count"] = 0; response["active_unread_count"] = 0;
                response["updated_at"] = "2026-01-02T10:01:00Z"; return {response, 200, 150, {}};
            }
            return original(r);
        };
        QVERIFY(f.start()); auto &c = f.controller; QTRY_COMPARE(c.messages().size(), 1);
        QSignalSpy markedRead(&c, &NativeController::conversationMarkedRead);
        c.markChatRead(streamId); QTRY_COMPARE(posts, 1);
        auto incoming = message("New while marking read", returnedId); incoming["read"] = false;
        incoming["created_at"] = "2026-01-02T10:02:00Z"; incoming["updated_at"] = "2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.created", incoming)));
        stream["updated_at"] = "2026-01-02T10:02:00Z"; stream["name"] = "Concurrent name";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", stream)));
        QTRY_VERIFY(!c.chatActionState(streamId).value("pending").toBool());
        QCOMPARE(c.streams().first().toMap().value("unread").toInt(), 1);
        QCOMPARE(c.streams().first().toMap().value("name").toString(), QString("Concurrent name"));
        QVERIFY(modelMessage(c, messageId).value("read").toBool());
        QVERIFY(!modelMessage(c, returnedId).isEmpty());
        QVERIFY(!modelMessage(c, returnedId).value("read").toBool());
        QCOMPARE(markedRead.size(), 1);
        QCOMPARE(markedRead.first().at(0).toString(), streamId);
        QVERIFY(markedRead.first().at(1).toString().isEmpty());
    }
    void sidebarReadDoesNotFollowAMovedMessageIntoAnotherStream()
    {
        Fixture f; auto stream = creationChannel(streamId, "Original"); stream["unread_count"] = 1;
        auto originalMessage = message(); originalMessage["read"] = false;
        auto moved = originalMessage; moved["stream_uuid"] = otherArchiveId; moved["topic_uuid"] = secondTopicId;
        moved["updated_at"] = "2026-01-02T10:02:00Z";
        f.streams = [&](const Request &) { return Response{QJsonArray{stream}}; };
        f.messages = [&](const Request &r) { return Response{QJsonArray{QUrlQuery(r.target).queryItemValue("stream_uuid") == otherArchiveId ? moved : originalMessage}}; };
        int posts = 0; const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &r) -> Response {
            if (r.target.path().endsWith("/actions/read/invoke")) { ++posts; auto result = stream; result["unread_count"] = 0; return {result, 200, 150, {}}; }
            return fallback(r);
        };
        QVERIFY(f.start()); auto &c = f.controller; QTRY_COMPARE(c.messages().size(), 1);
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.created", creationChannel(otherArchiveId, "Destination"))));
        c.markChatRead(streamId); QTRY_COMPARE(posts, 1);
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.updated", moved)));
        QTRY_VERIFY(!c.chatActionState(streamId).value("pending").toBool());
        f.aggregateMessages = true; c.selectStream(otherArchiveId); QTRY_COMPARE(c.messages().size(), 1);
        QCOMPARE(c.selectedStream(), otherArchiveId);
        QVERIFY(!modelMessage(c, messageId).isEmpty());
        QVERIFY2(!modelMessage(c, messageId).value("read").toBool(), "Reading the old stream cannot mark a moved placement in another stream as read");
    }

    void sidebarArchiveReconcilesLostAcknowledgement_data()
    {
        QTest::addColumn<bool>("applied"); QTest::newRow("applied") << true; QTest::newRow("not-applied") << false;
    }
    void sidebarArchiveReconcilesLostAcknowledgement()
    {
        QFETCH(bool, applied); Fixture f; auto stream = creationChannel(streamId, "Original");
        f.streams = [&](const Request &) { return Response{QJsonArray{stream}}; };
        int posts = 0, reads = 0; const auto original = f.server.handler;
        f.server.handler = [&](const Request &r) -> Response {
            if (r.target.path().endsWith("/actions/archive/invoke")) { ++posts; stream["is_archived"] = applied; return {QJsonObject{}, 503}; }
            if (r.target.path().endsWith("/streams/"+streamId) && r.method == "GET") { ++reads; return {stream}; }
            return original(r);
        };
        QVERIFY(f.start()); auto &c = f.controller; const auto initialReads = reads;
        c.archiveChat(streamId); QTRY_VERIFY(!c.chatActionState(streamId).value("pending").toBool());
        QCOMPARE(posts, 1); QCOMPARE(reads, initialReads + 1);
        QCOMPARE(c.archivedChats().size(), applied ? 1 : 0);
        if (!applied) QVERIFY(!c.chatActionState(streamId).value("error").toString().isEmpty());
    }

    void sidebarRenameRejectsEmptyUnchangedAndDirect()
    {
        Fixture f; auto stream = creationChannel(streamId, "Original");
        f.streams = [&](const Request &) { return Response{QJsonArray{stream}}; };
        QVERIFY(f.start()); const auto before = f.server.requests.size();
        f.controller.renameChat(streamId, "  "); f.controller.renameChat(streamId, " Original ");
        stream["private"] = true; stream["direct_user_uuid"] = otherArchiveId;
        QVERIFY(f.controller.applyRealtimeEvent(controllerEvent("stream.updated", stream)));
        f.controller.renameChat(streamId, "Different");
        QTest::qWait(50); QCOMPARE(f.server.requests.size(), before);
    }

    void archiveMetadataKindsAndForeignOwners_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"ordinary", "member", "direct", "encrypted", "provider", "foreign-user", "foreign-project"}) QTest::newRow(kind) << QString(kind);
    }
    void archiveMetadataKindsAndForeignOwners() {
        QFETCH(QString, kind); ArchiveFixture test;
        if (kind == "member") test.archived["role"] = "member";
        if (kind == "direct") { test.archived["direct_user_uuid"] = otherArchiveId; test.archived["private"] = true; }
        if (kind == "encrypted") { test.archived["encryption"] = true; test.archived["current_encryption_key_uuid"] = returnedId; }
        if (kind == "provider") { test.archived["source_name"] = "zulip"; test.archived["provider_uuid"] = returnedId; test.archived["provider_identity"] = "synthetic-stream"; }
        if (kind == "foreign-user") test.archived["user_uuid"] = otherArchiveId;
        if (kind == "foreign-project") test.archived["project_id"] = otherArchiveId;
        if (kind.startsWith("foreign")) {
            auto &c = test.fixture.controller; c.login(test.fixture.server.origin().toString(), "user", "password", "");
            QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId); QTRY_VERIFY(!c.error().isEmpty());
            QVERIFY(c.archivedChats().isEmpty()); QVERIFY(!c.canUnarchiveChat(archiveId)); return;
        }
        QVERIFY(test.start()); auto &c = test.fixture.controller; QCOMPARE(c.archivedChats().size(), 1);
        const auto row = c.archivedChats().first().toMap(); QCOMPARE(row.value("encrypted").toBool(), kind == "encrypted");
        QCOMPARE(row.value("isDirect").toBool(), kind == "direct"); QCOMPARE(row.value("name").toString(), QString("Archived channel"));
        QVERIFY(c.canUnarchiveChat(archiveId)); c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts, 1);
    }
    void archiveCanonicalResponseRequiresFullExactIdentity_data() {
        QTest::addColumn<QString>("field");
        for (const auto &field : {"uuid", "project_id", "user_uuid", "owner", "description", "role", "is_archived", "updated_at", "unread_count"}) QTest::newRow(field) << QString(field);
    }
    void archiveCanonicalResponseRequiresFullExactIdentity() {
        QFETCH(QString, field); ArchiveFixture test;
        const auto alter = [field](QJsonObject &row) {
            if (field == "description" || field == "unread_count") row.remove(field);
            else if (field == "is_archived") row[field] = "false";
            else if (field == "updated_at") row[field] = "not-a-date";
            else if (field == "role") row[field] = "invented-role";
            else if (field == "owner") row[field] = "not-a-uuid";
            else row[field] = otherArchiveId;
        };
        test.postDto = test.getDto = alter; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets, 1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        QCOMPARE(c.archivedChats().size(), 1); QCOMPARE(c.streams().size(), 1); QCOMPARE(test.posts, 1);
        QVERIFY(!c.archivedChats().first().toMap().value("error").toString().isEmpty());
    }
    void archiveAmbiguityReconcilesTheSameUuidWithoutAutomaticPost_data() {
        QTest::addColumn<bool>("applied"); QTest::newRow("applied") << true; QTest::newRow("not-applied") << false;
    }
    void archiveAmbiguityReconcilesTheSameUuidWithoutAutomaticPost() {
        QFETCH(bool, applied); ArchiveFixture test; test.postStatus = 503; test.applyPost = applied; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets, 1);
        if (applied) { QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts, 1); return; }
        QTRY_VERIFY(!archivedChat(c).value("pending").toBool()); QCOMPARE(test.posts, 1);
        test.getStatus = 503; c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets, 2);
        QTRY_VERIFY(!archivedChat(c).value("pending").toBool()); QCOMPARE(test.posts, 1);
        test.getStatus = test.postStatus = 200; test.applyPost = true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.gets, 3); QCOMPARE(test.posts, 2);
    }
    void archiveTerminalRefusalIsExplicitlyRetryable_data() {
        QTest::addColumn<int>("status"); for (int value : {400,401,403,404,422}) QTest::newRow(qPrintable(QString::number(value))) << value;
    }
    void archiveTerminalRefusalIsExplicitlyRetryable() {
        QFETCH(int, status); ArchiveFixture test; test.postStatus = status; test.applyPost = false; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        QCOMPARE(test.gets, 0); QVERIFY(c.canUnarchiveChat(archiveId)); test.postStatus = 200; test.applyPost = true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts, 2); QCOMPARE(test.gets, 0);
    }
    void archivePendingRowsAreIndependentAndHistoryRemainsOpenable() {
        ArchiveFixture test; test.includeOther = true; test.postDelay = 180; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); c.unarchiveChat(archiveId); c.unarchiveChat(otherArchiveId); QTRY_COMPARE(test.posts, 2);
        c.openArchivedChat(archiveId); QCOMPARE(c.selectedStream(), archiveId); QVERIFY(c.selectedTopic().isEmpty());
        QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(c.streams().size(), 3); QCOMPARE(c.selectedStream(), archiveId); QCOMPARE(test.posts, 2);
        for (const auto &request : test.fixture.server.requests) if (request.target.path().endsWith("/actions/unarchive/invoke")) {
            QCOMPARE(request.method, QByteArray("POST")); QVERIFY(request.body.isEmpty()); QCOMPARE(request.headers.value("content-length").toInt(), 0);
        }
    }
    void archiveNewerRealtimeAndUnrelatedFieldsWinOverDelayedResponse() {
        ArchiveFixture test; test.postDelay = 180; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.posts, 1);
        auto newer = test.archived; newer["is_archived"] = true; newer["updated_at"] = "2026-01-02T10:02:00.123456Z";
        newer["name"] = "Concurrent rename"; newer["unread_count"] = 7; newer["active_unread_count"] = 7; newer["notification_mode"] = "muted";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", newer)));
        QTRY_VERIFY(!archivedChat(c).value("pending").toBool()); QCOMPARE(c.archivedChats().size(), 1); QCOMPARE(c.streams().size(), 1);
        QCOMPARE(c.archivedChats().first().toMap().value("name").toString(), QString("Concurrent rename"));
        newer["is_archived"] = false; newer["updated_at"] = "2026-01-02T10:03:00Z"; QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", newer)));
        QCOMPARE(c.archivedChats().size(), 0); const auto row = c.streams().last().toMap(); QCOMPARE(row.value("unread").toLongLong(), 7); QCOMPARE(row.value("name").toString(), QString("Concurrent rename"));
    }
    void archiveDispatchAndRepliesAreFencedByOwnerTrustAndAccess_data() {
        QTest::addColumn<QString>("change"); QTest::addColumn<bool>("beforeDispatch");
        for (const auto &change : {"logout", "token", "refresh", "acl", "delete", "cache"})
            for (bool before : {false,true}) QTest::newRow(qPrintable(QString(change)+(before?"-dispatch":"-reply"))) << QString(change) << before;
    }
    void archiveDispatchAndRepliesAreFencedByOwnerTrustAndAccess() {
        QFETCH(QString, change); QFETCH(bool, beforeDispatch); ArchiveFixture test; test.postDelay = 120;
        test.applyPost = false; test.postDto = [](QJsonObject &row) { row["is_archived"] = false; row["updated_at"] = "2026-01-02T10:01:00Z"; };
        QVERIFY(test.start()); auto &c = test.fixture.controller;
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        bool changed = false;
        const auto mutate = [&] {
            if (changed) return; changed = true;
            if (change == "logout") c.logout();
            else if (change == "token") test.fixture.api.setAccessToken("replacement-generation");
            else if (change == "refresh") c.refresh();
            else if (change == "cache") c.useCache(&cache);
            else if (change == "delete") QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.deleted", {{"uuid", archiveId}})));
            else { auto row = test.archived; row["role"] = "member"; row["is_archived"] = true; QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", row))); }
        };
        QMetaObject::Connection observer;
        if (beforeDispatch) observer = connect(&c, &NativeController::streamsChanged, &c, mutate);
        c.unarchiveChat(archiveId);
        if (beforeDispatch) { disconnect(observer); QVERIFY(changed); QCOMPARE(test.posts, 0); }
        else { QTRY_COMPARE(test.posts, 1); mutate(); }
        // Wait until handled HTTP sockets settle; canceled generations may
        // close early. Idle keep-alive connections are not pending responses.
        QTRY_VERIFY(([&] { const auto sockets = test.fixture.server.findChildren<QTcpSocket *>();
            return std::none_of(sockets.cbegin(), sockets.cend(), [](QTcpSocket *socket) {
                return socket->property("handled").toBool() && socket->state() == QAbstractSocket::ConnectedState;
            }); })());
        if (change == "delete") QVERIFY(c.archivedChats().isEmpty());
        if (change == "logout") { QVERIFY(c.archivedChats().isEmpty()); QVERIFY(c.streams().isEmpty()); }
        for (const auto &row : c.streams()) QVERIFY(row.toMap().value("id").toString() != archiveId);
    }
    void archiveTopicActionsRemainVerifiedAndAccessLossStopsThem() {
        ArchiveFixture test; auto &c = test.fixture.controller;
        auto row = message("old text"); row["stream_uuid"] = archiveId; row["read"] = false;
        row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        QJsonObject sent; int sends = 0, edits = 0, deletes = 0, reads = 0, reacts = 0, reopens = 0;
        test.history = [&](const Request &) { return Response{QJsonArray{row}}; };
        test.fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/actions/read_up_to/invoke")) { ++reads; row["read"] = true; return {row}; }
            if (request.method == "POST" && path.endsWith("/messages/")) {
                ++sends; sent = message(request.body.value("payload").toObject().value("content").toString(), request.body.value("uuid").toString());
                sent["stream_uuid"] = archiveId; return {sent, 201};
            }
            if (request.method == "PUT") { ++edits; row["payload"] = request.body.value("payload"); return {row}; }
            if (request.method == "DELETE") { ++deletes; return {QJsonObject{}}; }
            return {path.endsWith(messageId) ? row : sent};
        };
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "thumbs_up"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        test.fixture.reactions = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++reacts; row["reactions"] = QJsonObject{{"thumbs_up",1}}; row["reaction_users"] = QJsonObject{{"thumbs_up",QJsonArray{userId}}}; return {fact,201}; }
            return {reacts ? QJsonArray{fact} : QJsonArray{}};
        };
        test.fixture.topicActions = [&](const Request &) { ++reopens; test.closed = false; return Response{creationTopic(topicId, archiveId, "Archived topic")}; };
        FileClient files(&test.fixture.api); c.useFiles(&files); QVERIFY(test.start());
        c.openArchivedChat(archiveId); QTRY_VERIFY(!c.busy()); QVERIFY(!c.canAttach()); QVERIFY(!c.canSend());
        QVERIFY(!c.canCreateTopicIn(archiveId)); c.selectTopic(topicId); QTRY_VERIFY(!c.busy());
        c.setDraft("new text"); QVERIFY(c.canSend()); QVERIFY(c.canAttach());
        c.markVisibleMessagesRead({messageId}); QTRY_COMPARE(reads, 1); QTRY_VERIFY(modelMessage(c,messageId).value("read").toBool());
        c.toggleReaction(messageId,"thumbs_up"); QTRY_COMPARE(reacts,1); QTRY_VERIFY(!modelMessage(c,messageId).value("reactionBusy").toBool());
        c.editMessage(messageId,"edited text"); QTRY_COMPARE(edits,1); QTRY_COMPARE(modelMessage(c,messageId).value("content").toString(),QString("edited text"));
        c.send(); QTRY_COMPARE(sends,1); QTRY_VERIFY(!c.busy()); QVERIFY(c.draft().isEmpty());
        c.deleteMessage(messageId); QTRY_COMPARE(deletes,1); QTRY_VERIFY(modelMessage(c,messageId).isEmpty());
        test.closed = true; auto closed = creationTopic(topicId,archiveId,"Archived topic"); closed["is_done"] = true;
        QVERIFY(c.applyRealtimeEvent(controllerEvent("topic.updated",closed))); c.setDraft("closed draft");
        QVERIFY(!c.canSend()); QVERIFY(!c.canAttach()); QVERIFY(c.canReopenTopic()); c.reopenTopic(); QTRY_COMPARE(reopens,1); QTRY_VERIFY(!c.topicClosed());
        QVERIFY(c.canSend()); QVERIFY(c.canAttach());
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.deleted",{{"uuid",archiveId}})));
        QVERIFY(c.selectedStream().isEmpty()); QVERIFY(!c.canSend()); QVERIFY(!c.canAttach()); QVERIFY(c.archivedChats().isEmpty());
        c.send(); c.reopenTopic(); c.deleteMessage(messageId); c.toggleReaction(messageId,"thumbs_up"); c.markVisibleMessagesRead({messageId});
        QCOMPARE(sends,1); QCOMPARE(reopens,1); QCOMPARE(deletes,1); QCOMPARE(reacts,1); QCOMPARE(reads,1);
    }
    void archiveAttachmentsAndDownloadsKeepTheirVerifiedStream() {
        ArchiveFixture test; FileClient files(&test.fixture.api); auto &c = test.fixture.controller; c.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt")); QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write("{}"),2); input.close();
        int uploads = 0, downloads = 0;
        test.fixture.files = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/download")) { ++downloads; return {QJsonObject{}}; }
            if (request.method == "POST") ++uploads;
            return {QJsonObject{{"uuid",returnedId},{"stream_uuid",archiveId},{"name","report.txt"},{"content_type","text/plain"},
                {"size_bytes",2},{"hash",QString::fromLatin1(QCryptographicHash::hash("{}",QCryptographicHash::Sha256).toHex())}}};
        };
        QVERIFY(test.start()); c.openArchivedChat(archiveId); QTRY_VERIFY(!c.busy());
        c.attachFiles({QUrl::fromLocalFile(input.fileName())}); QCOMPARE(uploads,0);
        c.selectTopic(topicId); QTRY_VERIFY(!c.busy()); QVERIFY(c.canAttach()); c.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!c.fileBusy()); QCOMPARE(uploads,1);
        // Upload-card contract: the verified stream's file reference is staged on the ready card
        // (it enters the outgoing body at send) instead of being appended to the visible draft.
        QCOMPARE(c.uploadEntries().size(), 1);
        const auto staged = c.uploadEntries().first().toMap();
        QCOMPARE(staged.value("status").toString(), QString("ready"));
        QVERIFY(staged.value("markdown").toString().contains("urn:file:"+returnedId));
        const auto destination=directory.filePath("saved.txt"); c.downloadFile(returnedId,QUrl::fromLocalFile(destination)); QTRY_VERIFY(!c.fileBusy());
        QCOMPARE(downloads,1); QFile output(destination); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(),QByteArray("{}"));
    }
    void archiveCatalogAndRestoreSurviveTheExistingAsyncCacheRestart() {
        QTemporaryDir directory; ArchiveFixture test;
        NativeCacheService first(directory.path(),{},true); test.fixture.controller.useCache(&first); QVERIFY(test.start()); auto &c = test.fixture.controller;
        QVERIFY(flushCache(first)); auto entry=cacheEntry(first,first.lease(),{NativeCache::Kind::Streams,{},{},{}}); QVERIFY(entry);
        QVERIFY(std::any_of(entry->rows.cbegin(),entry->rows.cend(),[](const QJsonValue &v){return v.toObject().value("uuid")==archiveId && v.toObject().value("is_archived").toBool();}));
        c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QVERIFY(flushCache(first));
        entry=cacheEntry(first,first.lease(),{NativeCache::Kind::Streams,{},{},{}}); QVERIFY(entry);
        bool found=false; for(const auto &v:entry->rows) if(v.toObject().value("uuid")==archiveId){found=true; QVERIFY(!v.toObject().value("is_archived").toBool()); QVERIFY(v.toObject().value("updated_at").toString().startsWith("2026-01-02T10:01:00"));} QVERIFY(found);
        c.useCache(nullptr); first.shutdown();
        NativeCacheService second(directory.path(),{},true); ApiClient api(nullptr,true); Session session(&api); NativeController restored(&api,&session); restored.useCache(&second);
        restored.login(test.fixture.server.origin().toString(),"user","password",""); QTRY_COMPARE(restored.projects().size(),1); restored.openProject(projectId);
        QTRY_COMPARE(restored.streams().size(),2); QVERIFY(restored.archivedChats().isEmpty()); QVERIFY(flushCache(second));
        QCOMPARE(test.posts,1);
    }
    void archiveTrustChangeNotifiesThePublishedRowGate() {
        ArchiveFixture test; QVERIFY(test.start()); auto &c=test.fixture.controller;
        QVERIFY(archivedChat(c).value("canUnarchive").toBool());
        const auto property=c.metaObject()->property(c.metaObject()->indexOfProperty("archivedChats"));
        QSignalSpy changes(&c,property.notifySignal()); QVERIFY(changes.isValid());
        test.fixture.api.setAccessToken("replacement-generation");
        QVERIFY(!archivedChat(c).value("canUnarchive").toBool()); QVERIFY(changes.count()>0);
    }
    void archiveUncertainRowReconcilesAgainAfterTheSameOwnerRefresh() {
        ArchiveFixture test; test.applyPost=false; test.postStatus=test.getStatus=503; QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets,1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        test.fixture.api.setAccessToken("replacement-generation"); c.refresh(); QTRY_VERIFY(c.canUnarchiveChat(archiveId));
        test.getStatus=test.postStatus=200; test.applyPost=true; c.unarchiveChat(archiveId);
        QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts,2); QCOMPARE(test.gets,2);
    }
    void archiveDirectLabelsUseStoredFallbackAndCurrentDirectoryNotifications() {
        ArchiveFixture test; test.archived["direct_user_uuid"]=channelMemberA; test.archived["private"]=true;
        QVERIFY(test.start()); auto &c=test.fixture.controller; QCOMPARE(archivedChat(c).value("name").toString(),QString("Archived channel"));
        QSignalSpy changes(&c,&NativeController::archivedChatsChanged);
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",{{"uuid",channelMemberA},{"username","alice"},{"first_name","Alice"}})));
        QCOMPARE(archivedChat(c).value("name").toString(),QString("Alice")); QVERIFY(changes.count()>0);
        c.logout(); QVERIFY(c.archivedChats().isEmpty());
    }
    void archiveSavedRowsAreVisibleBeforeTheDelayedCatalogButNeverMutable() {
        ArchiveFixture test; QTemporaryDir directory;
        { NativeCache seed(directory.path(),{},true); QVERIFY(seed.open(test.fixture.server.origin(),projectId,userId));
          QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Streams,{},{},{}},{creationChannel(streamId),test.archived})); }
        NativeCacheService cache(directory.path(),{},true); auto &c=test.fixture.controller; c.useCache(&cache); test.catalogDelay=600;
        c.login(test.fixture.server.origin().toString(),"user","password",""); QTRY_COMPARE(c.projects().size(),1); c.openProject(projectId);
        QTRY_VERIFY(c.cacheStale() && c.archivedChats().size()==1); QVERIFY(!c.canUnarchiveChat(archiveId));
        c.unarchiveChat(archiveId); QCOMPARE(test.posts,0); QCOMPARE(test.gets,0);
        QTRY_VERIFY(c.canUnarchiveChat(archiveId)); QCOMPARE(test.gets,0); QCOMPARE(test.posts,0);
    }
    void archiveRealtimeMovesTheSameRowWithoutDeletingHistoryOrItsTopicRoute() {
        ArchiveFixture test; test.fixture.messages=[](const Request &){return Response{QJsonArray{message()}};};
        QTemporaryDir directory; NativeCacheService cache(directory.path(),{},true); auto &c=test.fixture.controller; c.useCache(&cache); QVERIFY(test.start());
        QVERIFY(flushCache(cache)); const NativeCache::Key history{NativeCache::Kind::History,streamId,topicId,{}};
        QVERIFY(cacheEntry(cache,cache.lease(),history)); c.setDraft("Archive retains the actual topic");
        auto changed=creationChannel(streamId,"Active channel"); changed["is_archived"]=true; changed["updated_at"]="2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated",changed))); QCOMPARE(c.streams().size(),0); QCOMPARE(c.archivedChats().size(),2);
        QCOMPARE(c.selectedStream(),streamId); QCOMPARE(c.selectedTopic(),topicId); QVERIFY(c.canSend()); QVERIFY(!c.canCreateTopicIn(streamId));
        const auto saved=cacheEntry(cache,cache.lease(),history); QVERIFY(saved); QVERIFY(!saved->rows.isEmpty()); QVERIFY(!saved->fresh);
        changed["is_archived"]=false; changed["updated_at"]="2026-01-02T10:03:00Z"; QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated",changed)));
        QCOMPARE(c.streams().size(),1); QCOMPARE(c.archivedChats().size(),1); QCOMPARE(c.selectedStream(),streamId); QCOMPARE(test.posts,0);
    }
    void archiveReconciliationAccessLossCannotManufactureRows_data() {
        QTest::addColumn<int>("status"); QTest::newRow("forbidden")<<403; QTest::newRow("missing")<<404;
    }
    void archiveReconciliationAccessLossCannotManufactureRows() {
        QFETCH(int,status); ArchiveFixture test; test.postStatus=503; test.getStatus=status; QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.openArchivedChat(archiveId); QTRY_VERIFY(!c.busy()); c.unarchiveChat(archiveId);
        QTRY_VERIFY(c.archivedChats().isEmpty()); QVERIFY(c.selectedStream().isEmpty()); QCOMPARE(c.streams().size(),1); QCOMPARE(test.posts,1); QCOMPARE(test.gets,1);
        c.openArchivedChat(archiveId); c.unarchiveChat(archiveId); QCOMPARE(test.posts,1); QVERIFY(c.selectedStream().isEmpty());
    }
    void archiveUncertainRecoverySurvivesTerminalMapPressure() {
        ArchiveFixture test; test.applyPost=false; test.postStatus=test.getStatus=503;
        QStringList extra;
        for(int n=1;n<=128;++n) { const auto id=QString("92929292-9292-4292-8292-%1").arg(n,12,16,QLatin1Char('0'));
            auto row=creationChannel(id,"Archived"); row["is_archived"]=true; test.additional.insert(id,row); extra.append(id); }
        QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets,1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        test.postStatus=403;
        for(const auto &id:extra) { c.unarchiveChat(id); QTRY_VERIFY(!archivedChat(c,id).value("pending").toBool()); }
        QCOMPARE(test.posts,129); test.postStatus=test.getStatus=200; test.applyPost=true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(archivedChat(c).isEmpty()); QCOMPARE(test.posts,130); QCOMPARE(test.gets,2);
    }
    void archiveUnresolvedCapacityNeverDropsAnUnknownOutcome() {
        ArchiveFixture test; test.applyPost=false; test.postStatus=test.getStatus=503;
        QStringList extra;
        for(int n=1;n<=128;++n) { const auto id=QString("93939393-9393-4393-8393-%1").arg(n,12,16,QLatin1Char('0'));
            auto row=creationChannel(id,"Archived"); row["is_archived"]=true; test.additional.insert(id,row); extra.append(id); }
        QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets,1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        for(int n=0;n<127;++n) { c.unarchiveChat(extra[n]); QTRY_VERIFY(!archivedChat(c,extra[n]).value("pending").toBool()); }
        QCOMPARE(test.posts,128); QCOMPARE(test.gets,128);
        QVERIFY(!c.canUnarchiveChat(extra.last())); QVERIFY(!archivedChat(c,extra.last()).value("canUnarchive").toBool());
        c.unarchiveChat(extra.last()); QCOMPARE(test.posts,128); QCOMPARE(test.gets,128);
        // Full capacity must not prevent reconciliation of an existing row.
        QVERIFY(c.canUnarchiveChat(archiveId)); test.getStatus=test.postStatus=200; test.applyPost=true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(archivedChat(c).isEmpty()); QCOMPARE(test.gets,129); QCOMPARE(test.posts,129);
        QVERIFY(c.canUnarchiveChat(extra.last())); QVERIFY(archivedChat(c,extra.last()).value("canUnarchive").toBool());
    }
    void archiveTargetAuthorityCannotRejoinAnOldReplyAfterAba() {
        ArchiveFixture test; test.applyPost=false; auto &c=test.fixture.controller;
        bool changed=false, admitted=false;
        test.postDto=[&](QJsonObject &canonical) {
            canonical["is_archived"]=false; canonical["updated_at"]="2026-01-02T10:01:00Z";
            if(changed)return;
            changed=true;
            auto altered=test.archived; altered["role"]="member";
            // Real HTTP has already admitted the command. Both owner-scoped
            // events arrive before its queued response, without a newer date
            // masking the authority lifetime that this test exercises.
            const bool lost=c.applyRealtimeEvent(controllerEvent("stream.updated",altered,31));
            const bool restored=c.applyRealtimeEvent(controllerEvent("stream.updated",test.archived,32));
            admitted=lost && restored;
        };
        QVERIFY(test.start()); c.unarchiveChat(archiveId);
        QTRY_VERIFY(changed); QVERIFY(admitted);
        QTRY_VERIFY(([&] { const auto sockets=test.fixture.server.findChildren<QTcpSocket *>();
            return std::none_of(sockets.cbegin(),sockets.cend(),[](QTcpSocket *socket) {
                return socket->property("handled").toBool() && socket->state()==QAbstractSocket::ConnectedState;
            }); })());
        QVERIFY2(!archivedChat(c).isEmpty(),"An authority tuple restored after ABA accepted the old canonical reply");
        QCOMPARE(test.posts,1); QCOMPARE(test.gets,0);
        QVERIFY(c.canUnarchiveChat(archiveId)); test.applyPost=true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(archivedChat(c).isEmpty());
        QCOMPARE(test.gets,1); QCOMPARE(test.posts,2);
    }
    void archiveRejectedProjectionCannotKeepThePreviousCatalogFresh_data() {
        QTest::addColumn<bool>("requiredFull");
        QTest::newRow("optional-full")<<false;QTest::newRow("both-queues-full")<<true;
    }
    void archiveRejectedProjectionCannotKeepThePreviousCatalogFresh() {
        QFETCH(bool,requiredFull);ArchiveFixture test; QTemporaryDir directory;
        QSemaphore release; std::atomic<bool> entered=false,block=false;
        NativeCacheService cache(directory.path(),{},true,[&] {
            if(block.exchange(false)) {entered=true;release.acquire();}
            return QDateTime::currentMSecsSinceEpoch();
        });
        auto &c=test.fixture.controller;c.useCache(&cache);QVERIFY(test.start());QVERIFY(flushCache(cache));
        const NativeCache::Key catalog{NativeCache::Kind::Streams,{},{},{}};
        const auto before=cacheEntry(cache,cache.lease(),catalog);QVERIFY(before);QVERIFY(before->fresh);
        auto unblock=qScopeGuard([&]{release.release();});block=true;
        QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::Users,{},{},{}},{QJsonObject{{"uuid",userId},{"username","self"}}}));
        QTRY_VERIFY(entered.load());
        int completed=0;
        for(int n=0;n<NativeCacheService::MaximumQueuedJobs-1;++n)
            QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::History,streamId,topicId,QByteArray::number(n)},
                              {message()}, {}, &c,[&](bool){++completed;}));
        int cleaned=0;
        if(requiredFull)for(int n=0;n<64;++n)
            QVERIFY(cache.invalidate(cache.lease(),NativeCache::Kind::History,archiveId,returnedId,&c,[&](bool okay){if(okay)++cleaned;}));
        c.unarchiveChat(archiveId);QTRY_VERIFY(c.archivedChats().isEmpty());
        QCOMPARE(test.posts,1);QCOMPARE(test.gets,0);
        if(requiredFull)QVERIFY(!cache.markStale(cache.lease(),NativeCache::Kind::Streams));
        QVERIFY2(!cache.put(cache.lease(),{NativeCache::Kind::History,streamId,topicId,"full-probe"},{message()}),
                 "The canonical local projection must meet the actual full optional queue");
        release.release();unblock.dismiss();QTRY_COMPARE(completed,NativeCacheService::MaximumQueuedJobs-1);
        if(requiredFull)QTRY_COMPARE(cleaned,64);
        QVERIFY(flushCache(cache));
        const auto saved=cacheEntry(cache,cache.lease(),catalog);QVERIFY(saved);
        QVERIFY2(!saved->fresh,"A rejected canonical local projection left the previous catalog authoritative");
    }
    void archiveProjectedCatalogCannotBecomeFreshWhenStaleAdmissionIsFull() {
        ArchiveFixture test; QTemporaryDir directory;
        QSemaphore release; std::atomic<bool> entered=false,block=false;
        NativeCacheService cache(directory.path(),{},true,[&] {
            if(block.exchange(false)) {entered=true;release.acquire();}
            return QDateTime::currentMSecsSinceEpoch();
        });
        auto &c=test.fixture.controller;c.useCache(&cache);QVERIFY(test.start());QVERIFY(flushCache(cache));
        auto unblock=qScopeGuard([&]{release.release();});block=true;
        QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::Users,{},{},{}},{QJsonObject{{"uuid",userId},{"username","self"}}}));
        QTRY_VERIFY(entered.load());
        int cleaned=0;
        for(int n=0;n<64;++n)QVERIFY(cache.invalidate(cache.lease(),NativeCache::Kind::History,streamId,topicId,&c,[&](bool okay){if(okay)++cleaned;}));
        QSignalSpy pressure(&cache,&NativeCacheService::errorChanged);
        c.unarchiveChat(archiveId);QTRY_VERIFY(c.archivedChats().isEmpty());
        QCOMPARE(test.posts,1);QCOMPARE(test.gets,0);
        // The fixed path does not need another control slot. Prove the same
        // required queue is still full with a separate rejected probe.
        QVERIFY(!cache.markStale(cache.lease(),NativeCache::Kind::Streams));
        QVERIFY2(pressure.size()>0,"The actual stale control must meet a full required queue");
        QVERIFY(cache.error().contains("cleanup queue is busy"));
        release.release();unblock.dismiss();QTRY_COMPARE(cleaned,64);QVERIFY(flushCache(cache));
        const auto saved=cacheEntry(cache,cache.lease(),{NativeCache::Kind::Streams,{},{},{}});QVERIFY(saved);
        bool projected=false;
        for(const auto &value:saved->rows)if(value.toObject()["uuid"].toString()==archiveId){projected=true;QVERIFY(!value.toObject()["is_archived"].toBool());}
        QVERIFY2(projected,"The actual canonical local projection must have reached the cache");
        QVERIFY2(!saved->fresh,"An admitted local projection became a fresh catalog when its separate stale control was rejected");
    }
    void archiveAccessCleanupCannotCrossARealSynchronousCacheError() {
        ArchiveFixture test; test.postStatus=503; test.getStatus=403; test.applyPost=false;
        QTemporaryDir firstDirectory,secondDirectory; QSemaphore release; std::atomic<bool> entered=false,block=false;
        NativeCacheService first(firstDirectory.path(),{},true,[&] {
            if(block.exchange(false)) { entered=true; release.acquire(); }
            return QDateTime::currentMSecsSinceEpoch();
        });
        NativeCacheService second(secondDirectory.path(),{},true); auto &c=test.fixture.controller; c.useCache(&first); QVERIFY(test.start()); QVERIFY(flushCache(first));
        const NativeCache::Key savedHistory{NativeCache::Kind::History,archiveId,topicId,{}};
        { NativeCache seed(secondDirectory.path(),{},true); QVERIFY(seed.open(test.fixture.server.origin(),projectId,userId));
          auto row=message(); row["stream_uuid"]=archiveId; QVERIFY(seed.put(seed.lease(),savedHistory,{row})); }
        auto unblock=qScopeGuard([&]{release.release();}); block=true;
        QVERIFY(first.put(first.lease(),{NativeCache::Kind::Users,{},{},{}},{QJsonObject{{"uuid",userId},{"username","self"}}}));
        QTRY_VERIFY(entered.load());
        for(int n=0;n<64;++n) QVERIFY(first.invalidate(first.lease(),NativeCache::Kind::History,streamId,topicId,&c,[](bool){}));
        bool switched=false;
        const auto observer=connect(&first,&NativeCacheService::errorChanged,&c,[&] {
            if(switched)return; switched=true; c.useCache(&second); c.selectStream(streamId);
        });
        c.unarchiveChat(archiveId); QTRY_VERIFY(switched); disconnect(observer); QTRY_VERIFY(second.available());
        QVERIFY(flushCache(second)); const auto retained=cacheEntry(second,second.lease(),savedHistory);
        QVERIFY2(retained.has_value(),"The old access-failure cleanup erased history in the replacement cache");
        QCOMPARE(c.selectedStream(),streamId); QCOMPARE(test.posts,1); QCOMPARE(test.gets,1);
    }
    void archiveCatalogKeepsTheActiveProjectionStable() {
        ArchiveFixture test; QVERIFY(test.start()); auto &c = test.fixture.controller;
        QCOMPARE(c.streams().size(), 1); QTRY_COMPARE(c.archivedChats().size(), 1);
        const auto row = c.archivedChats().first().toMap(); QCOMPARE(row.value("id").toString(), archiveId);
        QCOMPARE(row.value("name").toString(), QString("Archived channel")); QVERIFY(!row.value("isDirect").toBool());
        QVERIFY(c.canUnarchiveChat(archiveId)); QCOMPARE(test.posts, 0); QCOMPARE(test.gets, 0);
    }
    void archiveOpensTheExactAggregateWithoutMutation() {
        ArchiveFixture test; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.openArchivedChat(archiveId); QCOMPARE(c.selectedStream(), archiveId); QCOMPARE(c.conversationKind(), QString("stream"));
        QVERIFY(c.selectedTopic().isEmpty()); QTRY_VERIFY(!c.busy()); QVERIFY(!c.canSend()); QVERIFY(!c.canAttach());
        QCOMPARE(test.posts, 0); QVERIFY(!c.messages().isEmpty());
    }
    void archiveRestoresOnlyAfterTheCanonicalPost() {
        ArchiveFixture test; test.postDelay = 120; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); c.unarchiveChat(archiveId); QTRY_COMPARE(test.posts, 1);
        QCOMPARE(c.archivedChats().size(), 1); QVERIFY(c.archivedChats().first().toMap().value("pending").toBool());
        QTRY_COMPARE(c.archivedChats().size(), 0); QCOMPARE(c.streams().size(), 2);
        QCOMPARE(c.selectedStream(), streamId); QCOMPARE(c.selectedTopic(), topicId); QCOMPARE(test.gets, 0);
    }

    // docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md (issue #2): the reference preview line is
    // whitespace-normalised and clamped to 120 characters with an ASCII "..." marker
    // (workspace_ui 61dacef entities/messenger/messenger-topic-list.lib.ts:61-92 with
    // DEFAULT_WORKSPACE_MESSAGE_SUMMARY_OPTIONS maxLength 120).
    void sidebarPreviewSummaryCollapsesAndClamps()
    {
        QCOMPARE(NativeController::summarizeRowPreview(QString()), QString());
        QCOMPARE(NativeController::summarizeRowPreview(QStringLiteral("  a\n\n  b  ")),
            QStringLiteral("a b"));
        const QString exact(120, QLatin1Char('y'));
        QCOMPARE(NativeController::summarizeRowPreview(exact), exact);
        const auto clamped = NativeController::summarizeRowPreview(QString(140, QLatin1Char('x')));
        QCOMPARE(clamped.size(), 120);
        QVERIFY2(clamped.endsWith(QStringLiteral("...")), qPrintable(clamped));
        QCOMPARE(clamped.left(117), QString(117, QLatin1Char('x')));
    }

    // workspace_ui 61dacef shared/lib/datetime.lib.ts:65-82 formatMessageTimeRelative:
    // same local day -> HH:mm, previous local day -> the yesterday label alone (no clock),
    // anything older -> the compact day + short month with no year.
    void sidebarRowTimeFollowsReferenceFormatter()
    {
        const QLocale english(QLocale::English);
        const QDate today(2026, 11, 11);
        const QString yesterday = QStringLiteral("Yesterday");
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 11), QTime(9, 5)), english, today, yesterday),
            QStringLiteral("09:05"));
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 11), QTime(23, 59)), english, today, yesterday),
            QStringLiteral("23:59"));
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 10), QTime(23, 59)), english, today, yesterday),
            yesterday);
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 5), QTime(0, 0)), english, today, yesterday),
            QStringLiteral("Nov 5"));
        // The compact style carries no year, so a date one year back keeps the same shape.
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2025, 11, 5), QTime(0, 0)), english, today, yesterday),
            QStringLiteral("Nov 5"));
    }

    // workspace_ui 61dacef entities/messenger/messenger-sidebar.lib.ts:236-290: the default
    // sort mode is last_message and the comparator is group rank then activity descending,
    // with unknown activity last. Native has no stream pin DTO yet (issue #98).
    void sidebarStreamsSortByActivityDescending()
    {
        Fixture test;
        const auto second = QStringLiteral("33333333-3333-4333-8333-444444444444");
        const auto third = QStringLiteral("33333333-3333-4333-8333-555555555555");
        const auto streamRow = [](const QString &uuid, const QString &name, const QString &updatedAt) {
            QJsonObject row{{"uuid", uuid}, {"project_id", projectId}, {"name", name},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}};
            if (!updatedAt.isEmpty()) row["updated_at"] = updatedAt;
            return row;
        };
        test.streams = [&](const Request &) {
            return Response{QJsonArray{
                streamRow(streamId, "Oldest", QStringLiteral("2026-01-01T10:00:00Z")),
                streamRow(second, "Newest", QStringLiteral("2026-01-03T10:00:00Z")),
                streamRow(third, "Undated", QString())}};
        };
        test.controller.login(test.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([&] { return test.controller.projects().size() == 1; }, 2000)) { QFAIL("project list"); return; }
        test.controller.openProject(projectId);
        if (!QTest::qWaitFor([&] { return test.controller.streams().size() == 3; }, 2000)) { QFAIL("stream list"); return; }
        QStringList order;
        for (const auto &row : test.controller.streams())
            order.append(row.toMap().value("name").toString());
        const QStringList expected{"Newest", "Oldest", "Undated"};
        QCOMPARE(order, expected);
    }

    // A hydrated last message gives the row its sender, the summary and the row time; the
    // own-message sender is common.you (workspace_ui messenger-topic-list.lib.ts:169-172).
    void sidebarRowsExposePreviewAndRowTime_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("expected");
        QTest::newRow("markdown") << QString("markdown") << QString("Alpha line second line");
        QTest::newRow("poll") << QString("poll") << QStringLiteral("Which day? • Monday (1) • Tuesday (2) Where? • Office (3) • Online (0)");
    }

    void sidebarRowsExposePreviewAndRowTime()
    {
        QFETCH(QString, kind);
        QFETCH(QString, expected);
        Fixture test;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000abc");
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}}};
            return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}};
        };
        test.topics = [&](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}, {"is_done", false},
                {"is_default", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}}}};
        };
        test.messages = [&](const Request &) {
            auto row = kind == "poll" ? pollMessageDto(last, 3, true) : message("Alpha   line\n\nsecond line", last);
            row["created_at"] = "2026-01-02T10:00:05.000Z";
            return Response{QJsonArray{row}};
        };
        QVERIFY(test.start());
        const auto topicRow = test.controller.topics().first().toMap();
        QCOMPARE(topicRow.value("previewSender").toString(), QStringLiteral("You"));
        QCOMPARE(topicRow.value("previewText").toString(), expected);
        QCOMPARE(topicRow.value("previewKind").toString(), kind);
        QCOMPARE(topicRow.value("previewState").toString(), QStringLiteral("ready"));
        QVERIFY(!topicRow.value("previewLocked").toBool());
        // The row time is the last-message instant under the same formatter the contract
        // case pins; asserting against the current date keeps this case locale-correct.
        QCOMPARE(topicRow.value("rowTime").toString(), NativeController::formatRowTime(
            QDateTime(QDate(2026, 1, 2), QTime(10, 0, 5), Qt::UTC), QLocale::system(),
            QDate::currentDate(), QStringLiteral("Yesterday")));
        const auto streamRow = test.controller.streams().first().toMap();
        QCOMPARE(streamRow.value("previewText").toString(), expected);
        QCOMPARE(streamRow.value("rowTime").toString(), topicRow.value("rowTime").toString());
    }

    // A row whose last message is an E2EE payload the client cannot present previews as the
    // locked label branch (workspace_ui sidebar-workspace-message-preview.ui.tsx), so the
    // projection must not leak cipher text and must flag the row.
    void sidebarRowsMarkUnreadableEncryptedPreview()
    {
        Fixture test;
        test.encrypted = true;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000abd");
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", true}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}}};
            return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", true}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}};
        };
        test.messages = [&](const Request &) {
            auto row = message(QStringLiteral("AQIDZmFrZWxvY2tlZHBheWxvYWQ="), last);
            row["created_at"] = "2026-01-02T10:00:05.000Z";
            row["payload"] = QJsonObject{{"kind", "e2ee"},
                {"key_uuid", QStringLiteral("66666666-6666-4666-8666-666666666666")},
                {"content", "AQIDZmFrZWxvY2tlZHBheWxvYWQ="}};
            return Response{QJsonArray{row}};
        };
        // Both topic DTOs must carry the same last message or the row loses its pointer
        // before the projection runs.
        test.topics = [&](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}, {"is_done", false},
                {"is_default", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}}}};
        };
        test.counters = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/" + streamId))
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", true}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}};
            return Response{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}, {"is_done", false},
                {"is_default", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}}};
        };
        QVERIFY(test.start());
        const auto row = test.controller.topics().first().toMap();
        QVERIFY2(row.value("previewLocked").toBool(), qPrintable(row.value("previewText").toString()));
        QCOMPARE(row.value("previewState").toString(), QStringLiteral("locked"));
        QCOMPARE(row.value("previewKind").toString(), QStringLiteral("e2ee"));
        QVERIFY2(!row.value("previewText").toString().contains(QStringLiteral("ZmFrZQ")),
            qPrintable(row.value("previewText").toString()));
    }

    // docs/NATIVE_DESKTOP_SIDEBAR_ROW_STATES.md (issue #96): streams() rows carry the
    // measured reference row state. Direct status facts come from the partner user DTO
    // through the resolveWorkspaceStatusEmojiDisplay port (legacy emoji names map to their
    // symbol, shortcode-like strings are dropped, blank text trims away); the displayed
    // counter follows resolveWorkspaceDisplayedUnread (active first, gray passive only);
    // unread-mention index membership surfaces on the row; and a folder item's pinned_at
    // marks the row pinned inside that folder's scope only.
    void sidebarRowsProjectStatusMentionPassiveUnreadAndPin()
    {
        Fixture test;
        const auto direct = QStringLiteral("9d9d9d9d-9d9d-49d9-89d9-9d9d9d9d9d9d");
        const auto shortcode = QStringLiteral("9c9c9c9c-9c9c-49c9-89c9-9c9c9c9c9c9c");
        const auto workFolder = QStringLiteral("7e7e7e7e-7e7e-47e7-87e7-7e7e7e7e7e7e");
        test.users = [](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", userId}, {"username", "self"}},
                QJsonObject{{"uuid", channelMemberA}, {"username", "alice"},
                    {"status_emoji", "palm_tree"}, {"status_text", "  Out of office until Friday  "}},
                QJsonObject{{"uuid", channelMemberB}, {"username", "bob"},
                    {"status_emoji", "working_from_home"}, {"status_text", "Deep work"}}}};
        };
        const auto streamDto = [](const QString &uuid, const QString &name, int unread, int active, int passive) {
            return QJsonObject{{"uuid", uuid}, {"project_id", projectId}, {"name", name},
                {"encryption", false}, {"unread_count", unread}, {"active_unread_count", active},
                {"passive_unread_count", passive}, {"last_message_uuid", QJsonValue::Null},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        test.streams = [&](const Request &request) {
            if (request.method != "GET") return Response{QJsonObject{{"error", "method"}}, 405};
            QJsonArray rows{streamDto(streamId, "Native channel", 5, 5, 0)};
            auto directRow = streamDto(direct, "Alice Adams", 2, 0, 2);
            directRow["private"] = true; directRow["direct_user_uuid"] = channelMemberA;
            auto shortRow = streamDto(shortcode, "Bob", 0, 0, 0);
            shortRow["private"] = true; shortRow["direct_user_uuid"] = channelMemberB;
            rows.append(directRow); rows.append(shortRow);
            return Response{rows};
        };
        // The unread-mention index walk (mentioned=true&read=false) finds one unread
        // mention in the channel; ordinary history pages stay empty.
        test.aggregateMessages = true;
        test.mentions = [](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("mentioned") == QLatin1String("true"))
                return Response{QJsonArray{mentionMessage(1)}};
            return Response{QJsonArray{}};
        };
        const auto folderItem = [&](const QString &uuid, const QString &stream, const QJsonValue &pinnedAt) {
            return QJsonObject{{"uuid", uuid}, {"project_id", projectId}, {"folder_uuid", workFolder},
                {"user_uuid", userId}, {"stream_uuid", stream}, {"chat_type", "channel"},
                {"pinned_at", pinnedAt}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"created_at", "2026-01-02T10:00:00Z"},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        test.folders = [&](const Request &) {
            const QJsonObject work{
                {"uuid", workFolder}, {"project_id", projectId}, {"title", "Work"},
                {"unread_count", 0}, {"system_type", "custom"},
                {"folder_items", QJsonArray{
                    folderItem("70000000-0000-4000-8000-00000000000a", streamId,
                        QStringLiteral("2026-01-05T10:00:00Z")),
                    folderItem("70000000-0000-4000-8000-00000000000b", direct,
                        QJsonValue::Null)}},
                {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
            return Response{QJsonArray{work}};
        };
        QVERIFY(test.start({}, 3));
        const auto rowOf = [&test](const QString &uuid) {
            for (const auto &value : test.controller.sidebarStreams())
                if (value.toMap().value("id").toString() == uuid) return value.toMap();
            return QVariantMap{}; };
        const auto channel = rowOf(streamId);
        QVERIFY2(!channel.isEmpty(), "channel row missing from the sidebar projection");
        QCOMPARE(channel.value("displayedUnread").toInt(), 5);
        QVERIFY(!channel.value("unreadPassive").toBool());
        QVERIFY(!channel.value("pinned").toBool());
        QVERIFY(!test.controller.mentionsVisible());
        QTRY_COMPARE_WITH_TIMEOUT(test.controller.activityMentionsCount(), 1, 3000);
        QVERIFY(rowOf(streamId).value("hasMention").toBool());
        QCOMPARE(channel.value("statusEmoji").toString(), QString{});
        QCOMPARE(channel.value("statusText").toString(), QString{});
        const auto alice = rowOf(direct);
        QVERIFY2(!alice.isEmpty(), "direct row missing from the sidebar projection");
        QCOMPARE(alice.value("statusEmoji").toString(), QString::fromUtf8("\xF0\x9F\x8C\xB4"));
        QCOMPARE(alice.value("statusText").toString(), QStringLiteral("Out of office until Friday"));
        QCOMPARE(alice.value("displayedUnread").toInt(), 2);
        QVERIFY2(alice.value("unreadPassive").toBool(), "passive-only unread must be flagged");
        QVERIFY(!alice.value("hasMention").toBool());
        QVERIFY(!alice.value("pinned").toBool());
        QCOMPARE(rowOf(shortcode).value("statusEmoji").toString(), QString{});
        QCOMPARE(rowOf(shortcode).value("statusText").toString(), QStringLiteral("Deep work"));
        // The folder scope reflects its items' pinned_at for that folder only.
        test.controller.selectFolder(workFolder);
        const auto scoped = test.controller.sidebarStreams();
        QCOMPARE(scoped.size(), 2);
        QVERIFY2(rowOf(streamId).value("pinned").toBool(), "pinned_at folder item must mark the row pinned");
        QVERIFY(!rowOf(direct).value("pinned").toBool());
    }

    // docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md section 2: a row whose last message the client
    // does not hold in memory is filled asynchronously from the disk cache by the bounded
    // pump, and the projection re-emits once the body resolves. This is the same best-effort
    // rule the reference applies to messages it has not hydrated.
    void sidebarPreviewsHydrateFromTheCacheAsynchronously()
    {
        Fixture test;
        const auto hidden = QStringLiteral("88888888-8888-4888-8888-000000000b00");
        const auto second = QStringLiteral("33333333-3333-4333-8333-666666666666");
        const auto streamRow = [](const QString &uuid, const QString &name, const QJsonValue &last) {
            return QJsonObject{{"uuid", uuid}, {"project_id", projectId}, {"name", name},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}};
        };
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{streamRow(streamId, "Native channel", QJsonValue::Null),
                    streamRow(second, "Cached channel", QString(hidden))}};
            return Response{streamRow(streamId, "Native channel", QJsonValue::Null)};
        };
        // The hidden message is never served to client memory: only the cache holds it.
        test.messages = [](const Request &) { return Response{QJsonArray{}}; };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), {}, true);
        test.controller.useCache(&cache);
        // The scene has two streams, so the single-stream start() helper does not apply;
        // the login flow is driven the way the multi-stream order case drives it.
        test.controller.login(test.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(test.controller.projects().size(), 1);
        test.controller.openProject(projectId);
        QTRY_COMPARE(test.controller.streams().size(), 2);
        auto cached = message("Cached preview body", hidden);
        // The body row belongs to the captured owner, like every real Body write.
        cached["user_uuid"] = userId;
        bool stored = false;
        QVERIFY(cache.putMessageBody(cache.lease(), cached, &cache, [&stored](bool okay) { stored = okay; }));
        QVERIFY(QTest::qWaitFor([&stored] { return stored; }, 2000));
        // The first projections ran before the body existed, so the pump must resolve the
        // pointer through the cache and re-emit the row without another snapshot.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const auto &entry : test.controller.streams()) {
                const auto row = entry.toMap();
                if (row.value("id").toString() == second)
                    return row.value("previewText").toString() == QStringLiteral("Cached preview body")
                        && !row.value("rowTime").toString().isEmpty();
            }
            return false;
        })(), 5000);
        // A message the cache also lacks keeps the empty preview line instead of a spinner
        // or a stale value; the negative lookup is remembered, so projections stay silent.
        const auto unknown = QStringLiteral("88888888-8888-4888-8888-000000000b99");
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{streamRow(streamId, "Native channel", QJsonValue::Null),
                    streamRow(second, "Cached channel", QString(hidden)),
                    streamRow(unknown, "Unknown channel", QStringLiteral("99999999-9999-4999-8999-999999999999"))}};
            return Response{streamRow(streamId, "Native channel", QJsonValue::Null)};
        };
        test.controller.refresh();
        QTRY_COMPARE(test.controller.streams().size(), 3);
        for (const auto &entry : test.controller.streams()) {
            const auto row = entry.toMap();
            if (row.value("id").toString() == unknown) {
                QCOMPARE(row.value("previewText").toString(), QString());
                QCOMPARE(row.value("rowTime").toString(), QString());
            }
        }
    }

private:
    // --- sidebar preview memo lifecycle (issue #116) ---

    // Shared scene for the cases below: one stream with two topics. The second topic's row
    // keeps its last-message pointer at `last`, and the message DTO is only ever served to
    // the second topic's history page: selecting the first topic pushes the message out of
    // controller.messages() again, so the row projects from the memo cache - exactly the
    // state where the stale memo bug surfaced.
    static void previewMemoScene(Fixture &test, const QString &last,
        const std::function<QJsonObject()> &current,
        const std::function<void(const QString &)> &applyEdit = {})
    {
        const auto topicRow = [](const QString &id, const QString &name, const QJsonValue &pointer) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", name}, {"is_done", false}, {"is_default", false}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", pointer}, {"updated_at", "2026-01-02T10:00:05Z"}};
        };
        test.topics = [topicRow, last](const Request &) {
            return Response{QJsonArray{topicRow(topicId, "First topic", QJsonValue::Null),
                topicRow(secondTopicId, "Second topic", QString(last))}};
        };
        test.counters = [topicRow, last](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith(secondTopicId))
                return Response{topicRow(secondTopicId, "Second topic", QString(last))};
            if (path.endsWith(topicId))
                return Response{topicRow(topicId, "First topic", QJsonValue::Null)};
            return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", QJsonValue::Null}}};
        };
        test.messages = [last, current, applyEdit](const Request &request) {
            const auto path = request.target.path();
            if (request.method == "PUT" && path.endsWith("/messages/" + last)) {
                if (applyEdit)
                    applyEdit(request.body.value("payload").toObject().value("content").toString());
                return Response{current()};
            }
            if (request.method == "GET" && path.endsWith("/messages/" + last))
                return Response{current()}; // the successful edit's verification GET
            if (request.method == "DELETE" && path.endsWith("/messages/" + last))
                return Response{QJsonObject{}, 204};
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId) {
                QJsonArray rows;
                rows.append(current());
                return Response{rows};
            }
            return Response{QJsonArray{}}; // the first topic never held the pointer
        };
    }

    // Seeded login: the scene is driven the way the multi-stream hydration case drives
    // it, pausing after the catalogs so the disk body can be stored before any topic row
    // (the only pointer to the message in this scene) can arm a hydration read. Without
    // this ordering a negative lookup can settle first and mask the memo being tested.
    static bool startPreviewSceneWithSeed(NativeController &controller, const QUrl &origin,
        const std::function<void()> &seed)
    {
        controller.login(origin.toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([&] { return controller.projects().size() == 1; }, 2000)) return false;
        controller.openProject(projectId);
        if (!QTest::qWaitFor([&] { return controller.streams().size() == 1; }, 2000)) return false;
        seed();
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([&] { return !controller.topics().isEmpty(); }, 2000)) return false;
        controller.selectTopic(topicId);
        return QTest::qWaitFor([&] { return !controller.busy(); }, 2000);
    }

    static QString previewMemoText(const NativeController &controller, const QString &topic)
    {
        for (const auto &entry : controller.topics()) {
            const auto row = entry.toMap();
            if (row.value("id").toString() == topic) return row.value("previewText").toString();
        }
        return QStringLiteral("<missing topic row>");
    }

    static void seedPreviewBody(NativeCacheService &cache, const QString &last, const QString &body)
    {
        auto diskRow = message(body, last, secondTopicId);
        diskRow["user_uuid"] = userId; // captured owner, like every real Body write
        bool stored = false;
        QVERIFY(cache.putMessageBody(cache.lease(), diskRow, &cache,
            [&stored](bool okay) { stored = okay; }));
        QVERIFY(QTest::qWaitFor([&stored] { return stored; }, 2000));
    }

private slots:
    // The issue trigger itself: hydrate the memo from disk while the row is unselected,
    // confirm an edit inside the conversation, then navigate away. The memo must own the
    // edited body; before #116 the stale disk preview returned once the message left
    // controller.messages().
    void sidebarPreviewKeepsConfirmedEditAfterNavigatingAway()
    {
        Fixture test;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000c16");
        const QString disk = QStringLiteral("Memoized disk body");
        const QString edited = QStringLiteral("Confirmed edited body");
        auto content = disk;
        previewMemoScene(test, last, [&] { return message(content, last, secondTopicId); },
            [&content](const QString &text) { content = text; });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), {}, true);
        test.controller.useCache(&cache);
        QVERIFY(startPreviewSceneWithSeed(test.controller, test.server.origin(),
            [&] { seedPreviewBody(cache, last, disk); }));
        QTRY_VERIFY_WITH_TIMEOUT(previewMemoText(test.controller, secondTopicId) == disk, 5000);
        test.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!test.controller.messages().isEmpty());
        test.controller.editMessage(last, edited);
        QTRY_COMPARE(modelMessage(test.controller, last).value("content").toString(), edited);
        test.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(test.controller.messages().isEmpty(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(previewMemoText(test.controller, secondTopicId) == edited, 2000);
        // A superseded disk body may not sneak back into the memo afterwards.
        QTest::qWait(300);
        QCOMPARE(previewMemoText(test.controller, secondTopicId), edited);
    }

    // A live edit for an unselected conversation and a persisted negative lookup share the
    // memo table: the never-resolved pointer keeps its quiet memo (proven by the late-seeded
    // disk row below staying suppressed), and the confirmed event payload must replace it.
    void sidebarPreviewFollowsLiveEditOverNegativeMemo()
    {
        Fixture test;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000c17");
        const QString edited = QStringLiteral("Confirmed live edit");
        previewMemoScene(test, last, [&] { return message(edited, last, secondTopicId); });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), {}, true);
        test.controller.useCache(&cache);
        QVERIFY(test.start());
        QTRY_VERIFY(cache.available());
        // Project the unresolved pointer once so the bounded pump resolves the miss.
        previewMemoText(test.controller, secondTopicId);
        QTest::qWait(400);
        seedPreviewBody(cache, last, QStringLiteral("Late disk body"));
        QTest::qWait(300);
        QCOMPARE(previewMemoText(test.controller, secondTopicId), QString()); // negative memo held
        QVERIFY(test.controller.applyRealtimeEvent(controllerEvent("message.updated",
            message(edited, last, secondTopicId), 4317)));
        QTRY_VERIFY_WITH_TIMEOUT(previewMemoText(test.controller, secondTopicId) == edited, 3000);
    }

    // A disk read issued while the memo is empty must not land after a live update and
    // re-memoize its stale body. The fence bumped by the event drops that read; any later
    // hydration resolves against the canonical row the event projected.
    void stalePreviewReadCannotOverwriteLiveEdit()
    {
        Fixture test;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000c18");
        const QString disk = QStringLiteral("Stale disk body");
        const QString edited = QStringLiteral("Confirmed live edit");
        previewMemoScene(test, last, [&] { return message(edited, last, secondTopicId); });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), {}, true);
        test.controller.useCache(&cache);
        QVERIFY(test.start());
        QTRY_VERIFY(cache.available());
        seedPreviewBody(cache, last, disk);
        // Start a read from the empty memo table, then confirm the live edit while it is
        // in flight. The row may only settle on the edited body, never the stale disk row.
        previewMemoText(test.controller, secondTopicId);
        QTest::qWait(2);
        QVERIFY(test.controller.applyRealtimeEvent(controllerEvent("message.updated",
            message(edited, last, secondTopicId), 4318)));
        QTRY_VERIFY_WITH_TIMEOUT(previewMemoText(test.controller, secondTopicId) == edited, 3000);
        QTest::qWait(300);
        QCOMPARE(previewMemoText(test.controller, secondTopicId), edited);
    }

    // A confirmed deletion carries the same memo lifecycle: the row keeps its pointer at
    // the deleted uuid, so the memo must go quiet right away - a deleted body may not keep
    // rendering from the memo, and re-hydration may not resurrect it while it rests on disk.
    void sidebarPreviewQuietsAfterConfirmedDelete()
    {
        Fixture test;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000c19");
        const QString disk = QStringLiteral("Deleted disk body");
        previewMemoScene(test, last, [&] { return message(disk, last, secondTopicId); });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), {}, true);
        test.controller.useCache(&cache);
        QVERIFY(startPreviewSceneWithSeed(test.controller, test.server.origin(),
            [&] { seedPreviewBody(cache, last, disk); }));
        QTRY_VERIFY_WITH_TIMEOUT(previewMemoText(test.controller, secondTopicId) == disk, 5000);
        test.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!test.controller.messages().isEmpty());
        test.controller.deleteMessage(last);
        QTRY_VERIFY_WITH_TIMEOUT(test.controller.messages().isEmpty(), 2000);
        test.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(test.controller.messages().isEmpty(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(previewMemoText(test.controller, secondTopicId).isEmpty(), 2000);
        QTest::qWait(400);
        QCOMPARE(previewMemoText(test.controller, secondTopicId), QString());
    }

    // --- media viewer (issue #8, docs/NATIVE_DESKTOP_MEDIA_VIEWER.md) ---

    void mediaViewerGalleryCollectsImageSlotsAndRejectsUnknownFile()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVERIFY(viewer);
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA)
             << mediaViewerRow("m2", mvFileB)
             << mediaViewerRow("m3", mvFileC, QStringLiteral("e2ee"))
             << mediaViewerRow("m4", mvFileA)
             << QVariantMap{{"id", "m5"}, {"kind", "markdown"}, {"content", "No media here"}, {"displayContent", "No media here"}};
        QVERIFY(!viewer->openFromMessages(rows, QStringLiteral("dddddddd-0000-4000-8000-00000000dd04")));
        QVERIFY(!viewer->open());
        QVERIFY(!viewer->openFromMessages(QVariantList{}, mvFileA));
        QVERIFY(viewer->openFromMessages(rows, mvFileB));
        QVERIFY(viewer->open());
        QCOMPARE(viewer->count(), 2);
        QCOMPARE(viewer->index(), 1);
        const auto items = viewer->items();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.at(0).toMap().value("uuid").toString(), mvFileA);
        QCOMPARE(items.at(1).toMap().value("uuid").toString(), mvFileB);
        // Reopening replaces the session and re-anchors the start index.
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QVERIFY(viewer->open());
        QCOMPARE(viewer->count(), 2);
        QCOMPARE(viewer->index(), 0);
        viewer->close();
        QVERIFY(!viewer->open());
        QCOMPARE(viewer->count(), 0);
    }

    void mediaViewerNavigationClampsAtBothEndsWithoutWrapAround()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerRow("m2", mvFileB) << mediaViewerRow("m3", mvFileC);
        QVERIFY(viewer->openFromMessages(rows, mvFileB));
        QCOMPARE(viewer->index(), 1);
        QVERIFY(viewer->canPrev());
        QVERIFY(viewer->canNext());
        viewer->prev();
        QCOMPARE(viewer->index(), 0);
        QVERIFY(!viewer->canPrev());
        viewer->prev();
        QCOMPARE(viewer->index(), 0);
        viewer->goTo(9);
        QCOMPARE(viewer->index(), 2);
        QVERIFY(!viewer->canNext());
        viewer->next();
        QCOMPARE(viewer->index(), 2);
        viewer->goTo(-4);
        QCOMPARE(viewer->index(), 0);
    }


    void folderRailModelPinsAllFirstAndCarriesServerFolderState()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(f.customUuid, "Focus", "created", 7, 0x2563eb, {folderRailItem(f.streamB)}),
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(channelsFolder, "Channels", "channels", 0, 0)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto folders = f.controller.folders();
        QCOMPARE(folderKeys(folders), (QStringList{"all", "personal", "channels", f.customUuid}));
        QCOMPARE(folders.first().toMap().value("id").toString(), allChatsFolder);
        const auto custom = folders.last().toMap();
        QCOMPARE(custom.value("title").toString(), QStringLiteral("Focus"));
        QCOMPARE(custom.value("systemType").toString(), QStringLiteral("created"));
        QCOMPARE(custom.value("unreadCount").toInt(), 7);
        QCOMPARE(custom.value("color").toUInt(), 0x2563ebu);
        QCOMPARE(custom.value("streamIds").toStringList(), (QStringList{f.streamB}));
        QCOMPARE(custom.value("synthesized").toBool(), false);
        QCOMPARE(folders.at(1).toMap().value("synthesized").toBool(), true);
    }

    void folderRailForcesSystemTypesFromFixedUuidsAndAcceptsCamelCase()
    {
        FolderRailFixture f;
        // The Personal UUID reported as "all" must still be personal; a custom
        // UUID reporting "all" degrades to created (reference forcing rule).
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", QString(), 0, 0, {}, true),
            folderRailFolder(personalFolder, "Personal", "all", 3, 0, {}, true),
            folderRailFolder(f.customUuid, "Ghost", "all", 0, 0, {}, true)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto folders = f.controller.folders();
        QCOMPARE(folderKeys(folders).first(), QStringLiteral("all"));
        for (const auto &value : folders)
        {
            const auto folder = value.toMap();
            if (folder.value("id").toString() == personalFolder)
            {
                QCOMPARE(folder.value("systemType").toString(), QStringLiteral("personal"));
                QCOMPARE(folder.value("unreadCount").toInt(), 3);
            }
            if (folder.value("id").toString() == f.customUuid)
                QCOMPARE(folder.value("systemType").toString(), QStringLiteral("created"));
        }
    }

    void folderRailSynthesizesSystemSlotsAndStaysQuietWhenFoldersUnavailable()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonObject{{"detail", "no"}} , 500};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 3, 4000);
        QCOMPARE(folderKeys(f.controller.folders()), (QStringList{"all", "personal", "channels"}));
        QVERIFY2(f.controller.error().isEmpty(), qPrintable(f.controller.error()));
        // Synthesized slots carry no server membership: the sidebar stays unfiltered.
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{streamId, f.streamB}));
        f.controller.selectFolder("personal");
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{streamId, f.streamB}));
    }

    void folderSelectionScopesSidebarStreamsAndSignalsOnlyOnChange()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb,
                {folderRailItem(f.streamB)})}};
        QVERIFY(f.start());
        // all + custom from the server, synthesized personal + channels pair.
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        QCOMPARE(f.controller.selectedFolder(), allChatsFolder);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{streamId, f.streamB}));
        QSignalSpy selected(&f.controller, &NativeController::folderSelected);
        f.controller.selectFolder(f.customUuid);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(f.controller.selectedFolder(), f.customUuid);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{f.streamB}));
        f.controller.selectFolder(f.customUuid); // re-selecting the effective folder: no signal
        QCOMPARE(selected.size(), 1);
        f.controller.selectFolder("all");
        QCOMPARE(selected.size(), 2);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{streamId, f.streamB}));
    }

    void folderRailFallsBackToAllWhenTheSelectedFolderDisappears()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb,
                {folderRailItem(f.streamB)})}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000); // all + custom + synthesized pair
        f.controller.selectFolder(f.customUuid);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{f.streamB}));
        // The folder vanishes on refresh: the effective folder must fall back to All.
        f.folders = Response{QJsonArray{folderRailFolder(allChatsFolder, "All chats", "all", 0, 0)}};
        f.controller.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 3, 4000); // all + synthesized pair
        QCOMPARE(f.controller.selectedFolder(), allChatsFolder);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{streamId, f.streamB}));
    }

    // Review finding (cid 4577604519): the loader must walk every page of the
    // paginated /folders/ endpoint before publishing, so folders beyond the
    // first page reach the rail instead of silently vanishing.
    void folderRailLoaderFollowsPaginationToTheLastPage()
    {
        FolderRailFixture f;
        const QString laterUuid = QStringLiteral("7a7a7a7a-7a7a-4a7a-8a7a-7a7a7a7a7a7a");
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (!request.target.path().endsWith("/messenger/folders/")) return fallback(request);
            if (QUrlQuery(request.target).queryItemValue("page_marker").isEmpty())
            {
                Response first{QJsonArray{folderRailFolder(f.customUuid, "Focus", "created", 0, 0)}};
                first.headers = {{QByteArrayLiteral("X-Pagination-Marker"), QByteArrayLiteral("page-2")}};
                return first;
            }
            return Response{QJsonArray{folderRailFolder(laterUuid, "Later", "created", 0, 0)}};
        };
        QVERIFY(f.start());
        // All + synthesized Personal/Channels plus the server folders of both pages.
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 5, 4000);
        QCOMPARE(folderKeys(f.controller.folders()),
                 (QStringList{"all", "personal", "channels", f.customUuid, laterUuid}));
        // The second page is requested with the marker the first page handed back.
        int pageTwoRequests = 0;
        for (const auto &request : f.server.requests)
            if (request.target.path().endsWith("/messenger/folders/")
                && QUrlQuery(request.target).queryItemValue("page_marker") == QLatin1String("page-2"))
                ++pageTwoRequests;
        QCOMPARE(pageTwoRequests, 1);
    }

    // Review finding (cid 4577604521): the store exposes folder items with the
    // camelCase streamUuid; accepting only stream_uuid leaves the membership
    // empty and hides the folder's chats.
    void folderRailAcceptsCamelCaseStreamUuidInsideFolderItems()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0, {}, true),
            QJsonObject{{"uuid", f.customUuid}, {"title", "Focus"}, {"systemType", "created"},
                {"unreadCount", 0}, {"backgroundColorValue", 0x2563eb},
                {"folderItems", QJsonArray{QJsonObject{{"uuid", "fi-b"}, {"streamUuid", f.streamB},
                    {"chatType", "channel"}}}}}}};
        QVERIFY(f.start());
        // all + custom from the server, synthesized personal + channels pair.
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto custom = f.controller.folders().last().toMap();
        QCOMPARE(custom.value("id").toString(), f.customUuid);
        QCOMPARE(custom.value("streamIds").toStringList(), (QStringList{f.streamB}));
        f.controller.selectFolder(f.customUuid);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{f.streamB}));
    }

    // Review finding (cid 4577604531): the positional "first untyped row is
    // All" rule is a legacy shape for servers without a system row; once the
    // fixed All UUID is present, an untyped foreign UUID must resolve to
    // created or both rows take the All key and the custom folder becomes
    // unselectable from the rail.
    void folderRailKeepsTheFixedAllRowWhenAFirstUntypedFolderArrives()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            // First row carries no system_type at all, next to the fixed All row.
            QJsonObject{{"uuid", f.customUuid}, {"title", "Focus"},
                {"unread_count", 0}, {"background_color_value", 0x2563eb}},
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto folders = f.controller.folders();
        // The fixed All UUID owns the All key; the untyped foreign UUID is created.
        QCOMPARE(folderKeys(folders), (QStringList{"all", "personal", "channels", f.customUuid}));
        QCOMPARE(folders.first().toMap().value("id").toString(), allChatsFolder);
        const auto custom = folders.last().toMap();
        QCOMPARE(custom.value("systemType").toString(), QStringLiteral("created"));
        QCOMPARE(custom.value("key").toString(), f.customUuid);
        // The custom folder stays selectable through its own rail key.
        f.controller.selectFolder(f.customUuid);
        QCOMPARE(f.controller.selectedFolder(), f.customUuid);
        // Legacy shape without a fixed All row keeps the positional fallback:
        // the first untyped row still resolves to All.
        FolderRailFixture legacy;
        legacy.folders = Response{QJsonArray{
            QJsonObject{{"uuid", legacy.customUuid}, {"title", "Legacy all"},
                {"unread_count", 0}, {"background_color_value", 0}}}};
        QVERIFY(legacy.start());
        QTRY_COMPARE_WITH_TIMEOUT(legacy.controller.folders().size(), 3, 4000);
        QCOMPARE(folderKeys(legacy.controller.folders()).first(), QStringLiteral("all"));
        QCOMPARE(legacy.controller.folders().first().toMap().value("id").toString(), legacy.customUuid);
    }

    // ---- Issue #84: folder create/rename/delete (docs/NATIVE_DESKTOP_FOLDERS.md) ----

    // handleCreate contract (folder-rail.ui.tsx): POST /folders/ with
    // {title, background_color_value}; the rail appends the created folder
    // without a full refetch.
    void folderCreateSendsContractRequestAndAppendsWithoutRefetch()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto fallback = f.server.handler;
        const QString createdUuid = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
        QJsonArray createdBodies;
        f.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path.endsWith("/messenger/folders/")) {
                createdBodies.append(request.body);
                return Response{QJsonObject{{"uuid", createdUuid},
                    {"title", request.body.value("title").toString()},
                    {"background_color_value", request.body.value("background_color_value")}}};
            }
            return fallback(request);
        };
        f.controller.createFolder("QA", 0xff0d9488);
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 5, 4000);
        QCOMPARE(createdBodies.size(), 1);
        const auto body = createdBodies.first().toObject();
        QCOMPARE(body.value("title").toString(), QStringLiteral("QA"));
        // The wire shape is the reference ARGB positive integer.
        QCOMPARE(body.value("background_color_value").toVariant().toString(), QStringLiteral("4279080072"));
        const auto row = f.controller.folders().last().toMap();
        QCOMPARE(row.value("key").toString(), createdUuid);
        QCOMPARE(row.value("id").toString(), createdUuid);
        QCOMPARE(row.value("title").toString(), QStringLiteral("QA"));
        QCOMPARE(row.value("systemType").toString(), QStringLiteral("created"));
        // Projections carry the RGB part only (folderColorValueToCssHex mask).
        QCOMPARE(row.value("color").toUInt(), quint32(0x000d9488));
        // No extra GET /folders/ happened: the append is incremental.
        int folderGets = 0;
        for (const auto &request : f.server.requests)
            if (request.method == "GET" && request.target.path().endsWith("/messenger/folders/")) ++folderGets;
        QCOMPARE(folderGets, 1);
        const auto mutation = f.controller.folderMutation();
        QVERIFY(mutation.value("error").toString().isEmpty());
        QVERIFY(!mutation.value("pending").toBool());
    }

    // handleSaveRename contract: PUT /folders/<uuid> with the same body
    // shape; the row updates in place, selection intact, no refetch.
    void folderRenameSendsPutAndUpdatesRowInPlace()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb,
                {folderRailItem(f.streamB)})}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        f.controller.selectFolder(f.customUuid);
        QCOMPARE(f.controller.selectedFolder(), f.customUuid);
        const auto fallback = f.server.handler;
        QJsonArray putBodies;
        f.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "PUT" && path.endsWith("/messenger/folders/" + f.customUuid)) {
                putBodies.append(request.body);
                return Response{QJsonObject{{"uuid", f.customUuid},
                    {"title", request.body.value("title").toString()},
                    {"system_type", "created"}, {"unread_count", 0},
                    {"background_color_value", request.body.value("background_color_value")}}};
            }
            return fallback(request);
        };
        f.controller.renameFolder(f.customUuid, "Focus 2", 0xffdc2626);
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().last().toMap().value("title").toString(),
            QStringLiteral("Focus 2"), 4000);
        QCOMPARE(putBodies.size(), 1);
        const auto body = putBodies.first().toObject();
        QCOMPARE(body.value("title").toString(), QStringLiteral("Focus 2"));
        QCOMPARE(body.value("background_color_value").toVariant().toString(), QStringLiteral("4292617766"));
        const auto row = f.controller.folders().last().toMap();
        QCOMPARE(row.value("color").toUInt(), quint32(0x00dc2626));
        QCOMPARE(row.value("streamIds").toStringList(), (QStringList{f.streamB}));
        // Membership (hence the scoped sidebar) survived the rename.
        QCOMPARE(f.controller.selectedFolder(), f.customUuid);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{f.streamB}));
        int folderGets = 0;
        for (const auto &request : f.server.requests)
            if (request.method == "GET" && request.target.path().endsWith("/messenger/folders/")) ++folderGets;
        QCOMPARE(folderGets, 1);
        QVERIFY(f.controller.folderMutation().value("error").toString().isEmpty());
    }

    // Review finding (cid 4616033386): the projection folded "no color" and a
    // stored black (background_color_value 0x00000000) into the same RGB 0, so
    // the edit prefill could not tell them apart and a name-only rename
    // repainted black to the preset-0 blue. Unset must project as a null
    // variant (the reference adapter's `?? null`), black stays a numeric 0,
    // and the rename request resends the stored 0 instead of the default blue.
    void folderRenameKeepsStoredBlackApartFromUnsetColor()
    {
        const QString blankUuid = QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Noir", "created", 0, 0),
            QJsonObject{{"uuid", blankUuid}, {"title", "Blank"}, {"system_type", "created"},
                {"unread_count", 0}}}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 5, 4000);
        QVariant noirColor, blankColor;
        for (const auto &folder : f.controller.folders())
        {
            const auto row = folder.toMap();
            if (row.value("id").toString() == f.customUuid) noirColor = row.value("color");
            if (row.value("id").toString() == blankUuid) blankColor = row.value("color");
        }
        // Stored black survives as a numeric zero...
        QVERIFY(noirColor.isValid() && !noirColor.isNull());
        QCOMPARE(noirColor.toUInt(), 0u);
        // ...while a folder with no color at all projects null, not 0.
        QVERIFY(blankColor.isNull());
        // Request-shape half: renaming the black row carries the stored zero
        // back on the wire (not 0xff2563eb, the preset-0 blue).
        const auto fallback = f.server.handler;
        QJsonArray putBodies;
        f.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "PUT" && path.endsWith("/messenger/folders/" + f.customUuid)) {
                putBodies.append(request.body);
                return Response{QJsonObject{{"uuid", f.customUuid},
                    {"title", request.body.value("title").toString()},
                    {"system_type", "created"}, {"unread_count", 0},
                    {"background_color_value", request.body.value("background_color_value")}}};
            }
            return fallback(request);
        };
        f.controller.renameFolder(f.customUuid, "Noir 2", 0);
        QTRY_COMPARE_WITH_TIMEOUT(putBodies.size(), 1, 4000);
        const auto body = putBodies.first().toObject();
        QCOMPARE(body.value("title").toString(), QStringLiteral("Noir 2"));
        QCOMPARE(body.value("background_color_value").toVariant().toString(), QStringLiteral("0"));
        QVariant echoedColor;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            for (const auto &folder : f.controller.folders())
            {
                const auto row = folder.toMap();
                if (row.value("id").toString() != f.customUuid) continue;
                if (row.value("title").toString() != QStringLiteral("Noir 2")) return false;
                echoedColor = row.value("color");
                return true;
            }
            return false;
        })(), 4000);
        QVERIFY(echoedColor.isValid() && !echoedColor.isNull());
        QCOMPARE(echoedColor.toUInt(), 0u);
    }

    // confirmDelete contract: DELETE /folders/<uuid>; the row disappears and
    // the effective selection falls back to All without a refetch.
    void folderDeleteRemovesRowAndFallsBackToAll()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        f.controller.selectFolder(f.customUuid);
        QCOMPARE(f.controller.selectedFolder(), f.customUuid);
        const auto fallback = f.server.handler;
        QStringList deleted;
        f.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "DELETE" && path.startsWith("/api/workspace/v1/messenger/folders/")
                && !path.endsWith("/folders/")) {
                deleted.append(path);
                return Response{QJsonObject{}};
            }
            return fallback(request);
        };
        f.controller.deleteFolder(f.customUuid);
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 3, 4000);
        QCOMPARE(deleted.size(), 1);
        QVERIFY(deleted.first().endsWith("/folders/" + f.customUuid));
        QCOMPARE(f.controller.selectedFolder(), allChatsFolder);
        QCOMPARE(sidebarRowIds(f.controller.sidebarStreams()), (QStringList{streamId, f.streamB}));
        int folderGets = 0;
        for (const auto &request : f.server.requests)
            if (request.method == "GET" && request.target.path().endsWith("/messenger/folders/")) ++folderGets;
        QCOMPARE(folderGets, 1);
        QVERIFY(f.controller.folderMutation().value("error").toString().isEmpty());
    }

    // Context-menu gating (folder-rail-context-menu.lib.ts): rename/delete
    // are disabled for system folders; the controller mirrors the gate and
    // never issues the request. Failed mutations carry the reference toast
    // defaults (folder.createFailed / updateFailed / deleteFailed).
    void folderMutationsGateSystemFoldersAndSurfaceErrors()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(personalFolder, "Personal", "personal", 0, 0)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 3, 4000);
        f.controller.renameFolder(personalFolder, "Nope", 0xff2563eb);
        QCOMPARE(f.controller.folderMutation().value("error").toString(),
            QStringLiteral("Could not update folder"));
        f.controller.deleteFolder(personalFolder);
        QCOMPARE(f.controller.folderMutation().value("error").toString(),
            QStringLiteral("Could not delete folder"));
        f.controller.renameFolder(QStringLiteral("00000000-dead-dead-dead-deaddeadbeef"), "Ghost", 0);
        QCOMPARE(f.controller.folderMutation().value("error").toString(),
            QStringLiteral("Could not update folder"));
        f.controller.deleteFolder(allChatsFolder);
        QCOMPARE(f.controller.folderMutation().value("error").toString(),
            QStringLiteral("Could not delete folder"));
        for (const auto &request : f.server.requests)
            QVERIFY(request.method != "PUT" && request.method != "DELETE");
        // A failing create settles with the reference toast text and adds no row.
        const auto fallback = f.server.handler;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.method == "POST" && request.target.path().endsWith("/messenger/folders/"))
                return Response{QJsonObject{{"detail", "no"}}, 500};
            return fallback(request);
        };
        f.controller.createFolder("QA", 0xff2563eb);
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.folderMutation().value("error").toString()
            == QStringLiteral("Could not create folder"), 4000);
        QCOMPARE(f.controller.folders().size(), 3);
        QVERIFY(!f.controller.folderMutation().value("pending").toBool());
    }

    // Single-flight: while a mutation is in flight a second one is ignored,
    // matching the modal's disabled submit and the reference isDeletingFolder
    // re-entry guard in confirmDelete.
    void folderMutationsAreSingleFlightUntilSettled()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{folderRailFolder(allChatsFolder, "All chats", "all", 0, 0)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 3, 4000);
        const auto fallback = f.server.handler;
        int posts = 0;
        f.server.handler = [&](const Request &request) -> Response {
            if (request.method == "POST" && request.target.path().endsWith("/messenger/folders/")) {
                ++posts;
                Response deferred{QJsonObject{{"uuid", QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")},
                    {"title", request.body.value("title").toString()}}};
                deferred.delay = 250;
                return deferred;
            }
            return fallback(request);
        };
        f.controller.createFolder("First", 0xff2563eb);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        f.controller.createFolder("Second", 0xff4f46e5); // ignored: one mutation at a time
        QCOMPARE(f.controller.folderMutation().value("kind").toString(), QStringLiteral("create"));
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.folderMutation().value("pending").toBool(), 4000);
        QCOMPARE(posts, 1);
        QCOMPARE(f.controller.folders().size(), 4);
        QCOMPARE(f.controller.folders().last().toMap().value("title").toString(), QStringLiteral("First"));
    }

    // ---- Issue #423: session-renewal fencing of folder mutations ----
    // docs/NATIVE_DESKTOP_FOLDERS.md "Session-renewal fencing of folder
    // mutations": a same-owner token renewal bumps the API generation while
    // a folder create/rename/delete response is still in flight. The retired
    // transport callback can never settle, so the generation-change handler
    // must clear the single-flight admission flag, present a retryable
    // interrupted outcome (distinct from success), let the renewed session
    // dispatch a new operation, keep the late reply fenced, and refetch the
    // authoritative folder list because the interrupted write's result is
    // unknown.

    // Held create + generation bump: interrupted outcome, authoritative
    // refetch, and a fresh POST under the renewed session.
    void folderRenewalInterruptsHeldCreateAndAllowsRetry()
    {
        FolderRailFixture f;
        const auto serverRows = std::make_shared<QJsonArray>(QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb)});
        f.folders = Response{*serverRows};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto fallback = f.server.handler;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto posts = std::make_shared<int>(0);
        auto gets = std::make_shared<int>(0);
        f.server.handler = [=](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messenger/folders/")) {
                ++*gets;
                return Response{*serverRows};
            }
            if (request.method == "POST" && path.endsWith("/messenger/folders/")) {
                ++*posts;
                Response deferred{QJsonObject{{"uuid", QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc")},
                    {"title", request.body.value("title").toString()}}};
                deferred.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return deferred;
            }
            return fallback(request);
        };
        f.controller.createFolder("Interrupted", 0xff2563eb);
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 4000); // the create is in flight
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        QCOMPARE(f.controller.folderMutation().value("kind").toString(), QStringLiteral("create"));
        // Same-owner renewal: a replacement access token bumps the generation.
        f.api.setAccessToken("synthetic-renewed-access-token");
        // AC1: the held create settles as an interrupted outcome distinct from
        // success and from the createFailed toast, with pending released.
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            const auto mutation = f.controller.folderMutation();
            return mutation.value("kind").toString().isEmpty()
                && !mutation.value("pending").toBool()
                && mutation.value("error").toString()
                    == QStringLiteral("The folder change was interrupted by a session refresh. Try again.");
        })(), 4000);
        // The write's result is unknown, so the client refetches the
        // authoritative list under the renewed session: whatever the server
        // answers lands on the rail.
        const auto serverUuid = QStringLiteral("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
        serverRows->append(folderRailFolder(serverUuid, "Reconciled", "created", 0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            for (const auto &folder : f.controller.folders())
                if (folder.toMap().value("id").toString() == serverUuid) return true;
            return false;
        })(), 4000);
        QVERIFY(*gets >= 1);
        // AC2: the renewed session submits a new create without reset or
        // sign-out: a second POST actually dispatches.
        const auto gatesBefore = gates->size();
        const auto postsBefore = *posts;
        f.controller.createFolder("Retry", 0xff4f46e5);
        QTRY_VERIFY_WITH_TIMEOUT(*posts >= postsBefore + 1, 4000);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        QCOMPARE(f.controller.folderMutation().value("kind").toString(), QStringLiteral("create"));
        QVERIFY(f.controller.folderMutation().value("error").toString().isEmpty());
        // AC3: the retired generation's answer may not settle the new
        // operation; only the new request's own answer does.
        for (size_t i = 0; i < gatesBefore; ++i) (*gates)[i]();
        QTest::qWait(250); // give the retired callback a chance to misfire
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        for (size_t i = gatesBefore; i < gates->size(); ++i) (*gates)[i]();
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.folderMutation().value("pending").toBool(), 4000);
        QVERIFY(f.controller.folderMutation().value("error").toString().isEmpty());
    }

    // Held rename + generation bump: same contract on the PUT path; the
    // interrupted row keeps its old title and a renewed PUT gets dispatched.
    void folderRenewalInterruptsHeldRenameAndAllowsRetry()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto fallback = f.server.handler;
        const auto customUuid = f.customUuid;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto puts = std::make_shared<int>(0);
        f.server.handler = [=](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "PUT" && path.endsWith("/messenger/folders/" + customUuid)) {
                ++*puts;
                Response deferred{QJsonObject{{"uuid", customUuid},
                    {"title", request.body.value("title").toString()},
                    {"system_type", "created"}, {"unread_count", 0},
                    {"background_color_value", request.body.value("background_color_value")}}};
                deferred.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return deferred;
            }
            return fallback(request);
        };
        f.controller.renameFolder(f.customUuid, "Focus 2", 0xffdc2626);
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 4000); // the rename is in flight
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        f.api.setAccessToken("synthetic-renewed-access-token");
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            const auto mutation = f.controller.folderMutation();
            return mutation.value("kind").toString().isEmpty()
                && !mutation.value("pending").toBool()
                && mutation.value("error").toString()
                    == QStringLiteral("The folder change was interrupted by a session refresh. Try again.");
        })(), 4000);
        // The interrupted rename never repainted the row.
        for (const auto &folder : f.controller.folders())
            if (folder.toMap().value("id").toString() == f.customUuid)
                QCOMPARE(folder.toMap().value("title").toString(), QStringLiteral("Focus"));
        // AC2: the renewed session dispatches a fresh PUT.
        const auto gatesBefore = gates->size();
        const auto putsBefore = *puts;
        f.controller.renameFolder(f.customUuid, "Focus 3", 0xffdc2626);
        QTRY_VERIFY_WITH_TIMEOUT(*puts >= putsBefore + 1, 4000);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        QCOMPARE(f.controller.folderMutation().value("kind").toString(), QStringLiteral("rename"));
        // AC3: the retired rename answer may not settle the newer operation.
        for (size_t i = 0; i < gatesBefore; ++i) (*gates)[i]();
        QTest::qWait(250);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        for (size_t i = gatesBefore; i < gates->size(); ++i) (*gates)[i]();
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.folders().last().toMap().value("title").toString()
            == QStringLiteral("Focus 3"), 4000);
        QVERIFY(!f.controller.folderMutation().value("pending").toBool());
        QVERIFY(f.controller.folderMutation().value("error").toString().isEmpty());
    }

    // Held delete + generation bump: same contract on the DELETE path; the
    // row survives the interrupted delete and a renewed DELETE is dispatched.
    void folderRenewalInterruptsHeldDeleteAndAllowsRetry()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0),
            folderRailFolder(f.customUuid, "Focus", "created", 0, 0x2563eb)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 4, 4000);
        const auto fallback = f.server.handler;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto deletes = std::make_shared<int>(0);
        f.server.handler = [&, deletes](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "DELETE" && path.startsWith("/api/workspace/v1/messenger/folders/")
                && !path.endsWith("/folders/")) {
                ++*deletes;
                Response deferred{QJsonObject{}};
                deferred.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return deferred;
            }
            return fallback(request);
        };
        f.controller.deleteFolder(f.customUuid);
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 4000); // the delete is in flight
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        f.api.setAccessToken("synthetic-renewed-access-token");
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            const auto mutation = f.controller.folderMutation();
            return mutation.value("kind").toString().isEmpty()
                && !mutation.value("pending").toBool()
                && mutation.value("error").toString()
                    == QStringLiteral("The folder change was interrupted by a session refresh. Try again.");
        })(), 4000);
        // The row stays until the authoritative list says otherwise; the
        // fixture refetch keeps answering with the server truth (Focus in).
        bool rowSurvives = false;
        for (const auto &folder : f.controller.folders())
            if (folder.toMap().value("id").toString() == f.customUuid) rowSurvives = true;
        QVERIFY(rowSurvives);
        // AC2: the renewed session dispatches a fresh DELETE.
        const auto gatesBefore = gates->size();
        const auto deletesBefore = *deletes;
        f.controller.deleteFolder(f.customUuid);
        QTRY_VERIFY_WITH_TIMEOUT(*deletes >= deletesBefore + 1, 4000);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        QCOMPARE(f.controller.folderMutation().value("kind").toString(), QStringLiteral("delete"));
        // AC3: the retired delete answer may not settle the newer operation.
        for (size_t i = 0; i < gatesBefore; ++i) (*gates)[i]();
        QTest::qWait(250);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        for (size_t i = gatesBefore; i < gates->size(); ++i) (*gates)[i]();
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            for (const auto &folder : f.controller.folders())
                if (folder.toMap().value("id").toString() == f.customUuid) return false;
            return true;
        })(), 4000);
        QVERIFY(!f.controller.folderMutation().value("pending").toBool());
        QVERIFY(f.controller.folderMutation().value("error").toString().isEmpty());
    }

    // AC3 head-on: a late old-generation create reply lands while a NEWER
    // create is in flight after the renewal. It may neither settle the new
    // operation's single-flight state nor touch the folder rail; only the
    // newer request's own answer applies.
    void lateFolderMutationReplyCannotSettleNewOperationAfterRenewal()
    {
        FolderRailFixture f;
        f.folders = Response{QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0)}};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 3, 4000);
        const auto fallback = f.server.handler;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto posts = std::make_shared<int>(0);
        f.server.handler = [=](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path.endsWith("/messenger/folders/")) {
                ++*posts;
                const auto title = request.body.value("title").toString();
                Response deferred{QJsonObject{{"uuid",
                        title == QStringLiteral("Old") ? QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")
                                                      : QStringLiteral("ffffffff-ffff-4fff-8fff-ffffffffffff")},
                    {"title", title}}};
                deferred.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return deferred;
            }
            return fallback(request);
        };
        f.controller.createFolder("Old", 0xff2563eb);
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 4000); // the old create is in flight
        f.api.setAccessToken("synthetic-renewed-access-token");
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.folderMutation().value("pending").toBool(), 4000);
        // The renewed session submits a fresh create before the old reply lands.
        f.controller.createFolder("New", 0xff4f46e5);
        QTRY_VERIFY_WITH_TIMEOUT(*posts >= 2, 4000);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        // Release the old-generation reply: nothing about the new operation
        // changes and no old row appears on the rail.
        (*gates)[0]();
        QTest::qWait(250);
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        QCOMPARE(f.controller.folderMutation().value("kind").toString(), QStringLiteral("create"));
        for (const auto &folder : f.controller.folders())
            QVERIFY(folder.toMap().value("title").toString() != QStringLiteral("Old"));
        // The newer request's own answer settles it and only its row appears.
        for (size_t i = 1; i < gates->size(); ++i) (*gates)[i]();
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.folders().last().toMap().value("title").toString()
            == QStringLiteral("New"), 4000);
        QVERIFY(!f.controller.folderMutation().value("pending").toBool());
        for (const auto &folder : f.controller.folders())
            QVERIFY(folder.toMap().value("title").toString() != QStringLiteral("Old"));
    }

    // Review 4628637592: the single reconciliation GET can complete while the
    // interrupted write is still applying on the server (the renewal aborted
    // the client-side transport, so that late commit is invisible: the reply
    // never reaches the controller and folder events do not update the rail).
    // The controller must therefore also run one bounded follow-up reconcile
    // once the interrupted write can no longer race it, so the projection
    // ends up on the committed server state instead of predating it.
    void folderRenewalReconcilesAgainWhenInterruptedWriteCommitsLate()
    {
        FolderRailFixture f;
        const auto serverRows = std::make_shared<QJsonArray>(QJsonArray{
            folderRailFolder(allChatsFolder, "All chats", "all", 0, 0)});
        f.folders = Response{*serverRows};
        QVERIFY(f.start());
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.folders().size(), 3, 4000);
        const auto fallback = f.server.handler;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto gets = std::make_shared<int>(0);
        const auto lateUuid = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
        f.server.handler = [=](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messenger/folders/")) {
                ++*gets;
                return Response{*serverRows};
            }
            if (request.method == "POST" && path.endsWith("/messenger/folders/")) {
                const auto title = request.body.value("title").toString();
                Response deferred{QJsonObject{{"uuid", lateUuid}, {"title", title},
                    {"system_type", "created"}, {"unread_count", 0}, {"background_color_value", 0x2563eb}}};
                // The server applies the write at the moment its response
                // goes out: the commit becomes visible exactly when the
                // (already client-aborted) transport finishes.
                deferred.defer = [gates, serverRows, lateUuid, title](std::function<void()> send) {
                    gates->push_back([serverRows, lateUuid, title, send]() {
                        serverRows->append(folderRailFolder(lateUuid, title, "created", 0, 0x2563eb));
                        send();
                    });
                };
                return deferred;
            }
            return fallback(request);
        };
        const auto lateVisible = [&]{
            for (const auto &folder : f.controller.folders())
                if (folder.toMap().value("id").toString() == lateUuid) return true;
            return false;
        };
        f.controller.createFolder("Late", 0xff2563eb);
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 4000); // the create is in flight
        QVERIFY(f.controller.folderMutation().value("pending").toBool());
        f.api.setAccessToken("synthetic-renewed-access-token");
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            const auto mutation = f.controller.folderMutation();
            return mutation.value("kind").toString().isEmpty()
                && !mutation.value("pending").toBool()
                && mutation.value("error").toString()
                    == QStringLiteral("The folder change was interrupted by a session refresh. Try again.");
        })(), 4000);
        // The interrupt-time reconciliation completes while the write has not
        // committed yet: its answer came back without the created row.
        QTRY_VERIFY_WITH_TIMEOUT(*gets >= 1, 4000);
        QTest::qWait(250); // the reconciliation answer is processed onto the rail
        QCOMPARE(*gets, 1);
        QVERIFY(!lateVisible());
        // The retired write now finishes: the server commits, but the reply is
        // fenced at the client and folder events do not touch the rail.
        (*gates)[0]();
        // The projection must still end up on the committed state: exactly one
        // bounded follow-up reconcile re-reads the authoritative list once the
        // interrupted write can no longer change it.
        QTRY_VERIFY_WITH_TIMEOUT(lateVisible(), 7000);
        QVERIFY(*gets >= 2);
        // The fenced reply settled nothing about the operation state.
        const auto mutation = f.controller.folderMutation();
        QVERIFY(!mutation.value("pending").toBool());
        QVERIFY(mutation.value("kind").toString().isEmpty());
        QCOMPARE(mutation.value("error").toString(),
            QStringLiteral("The folder change was interrupted by a session refresh. Try again."));
        // Bounded: the follow-up reconciles once, not in a loop.
        const auto getsAfterFollowup = *gets;
        QTest::qWait(500);
        QCOMPARE(*gets, getsAfterFollowup);
    }

    // Issue #718: sidebar sort keys must be independent of the active message
    // window. A stream whose history was hydrated keeps its position when the
    // route moves away and the window is swapped; only real activity moves rows.
    void sidebarActivityOrderSurvivesLeavingTheHydratedStream()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString recent = "77777777-7777-4777-8777-000000000001";
        const QString bgA = "aaaaaaaa-aaaa-4aaa-8aaa-00000000000a";
        const QString bgB = "bbbbbbbb-bbbb-4bbb-8bbb-00000000000b";
        auto hero = message("Hero message", recent, topicId);
        hero["created_at"] = "2026-06-01T10:00:00Z";
        const auto streamDto = [](const QString &id, const QString &name,
                                  const QString &updated, const QString &last) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"name", name},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"updated_at", updated},
                {"last_message_uuid", last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(last)}};
        };
        const QJsonArray streams{streamDto(streamId, "Hydrated", "2026-01-01T00:00:00Z", recent),
            streamDto(bgA, "Alpha", "2026-01-02T00:00:00Z", QString()),
            streamDto(bgB, "Beta", "2026-01-03T00:00:00Z", QString())};
        fixture.streams = [&](const Request &) { return Response{streams}; };
        fixture.counters = [&](const Request &request) -> Response {
            const auto id = request.target.path().section('/', -1);
            for (const auto &value : streams)
                if (value.toObject().value("uuid").toString() == id) return Response{value.toObject()};
            return Response{QJsonObject{{"uuid", id}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", QJsonValue(QJsonValue::Null)}}};
        };
        fixture.topics = [&](const Request &request) -> Response {
            const auto parent = QUrlQuery(request.target).queryItemValue("stream_uuid");
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", parent.isEmpty() ? streamId : parent}, {"name", "Native topic"}}}};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.toString().contains(bgA) || request.target.toString().contains(bgB))
                return Response{QJsonArray{}};
            return Response{QJsonArray{hero}};
        };
        auto &c = fixture.controller;
        const auto position = [&](const QString &id) {
            const auto rows = c.streams();
            for (int i = 0; i < rows.size(); ++i)
                if (rows.at(i).toMap().value("id").toString() == id) return i;
            return -1;
        };
        c.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId);
        QTRY_COMPARE(c.streams().size(), 3);
        QCOMPARE(position(streamId), 2); // Oldest updated_at sorts last before hydration.
        c.selectStream(streamId);
        QTRY_COMPARE(c.messages().size(), 1); QTRY_VERIFY(!c.busy());
        QCOMPARE(position(streamId), 0); // The hydrated last-message stamp ranks it first.
        c.selectStream(bgA);
        QTRY_VERIFY(!c.busy());
        QTest::qWait(200);
        QCOMPARE(position(streamId), 0); // Leaving must not rewind the sort key.
        c.selectStream(streamId);
        QTRY_COMPARE(c.messages().size(), 1); QTRY_VERIFY(!c.busy());
        QCOMPARE(position(streamId), 0);
    }

    // Issue #718 slice 2: switching back to a warmed topic must paint the
    // topic's own rows on the first published frame instead of the empty
    // window, and the bounded background preload must warm unopened populated
    // topics exactly once each without hammering a failing one.
    void topicHistorySwapShowsPreparedRowsDuringDelayedReload()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString topicB = "bbbbbbbb-bbbb-4bbb-8bbb-00000000000b";
        const QString a1 = "aaaaaaaa-aaaa-4aaa-8aaa-0000000000b1";
        const QString a2 = "aaaaaaaa-aaaa-4aaa-8aaa-0000000000b2";
        const auto msgA1 = message("A one", a1, topicId);
        const auto msgA2 = message("A two", a2, topicId);
        const auto msgB = message("B one", "bbbbbbbb-bbbb-4bbb-8bbb-0000000000b1", topicB);
        const auto topicDto = [](const QString &id, const QString &name, const QString &last, bool isDefault) {
            QJsonObject row{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", name}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"updated_at", "2026-01-02T10:00:00Z"},
                {"last_message_uuid", last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(last)}};
            if (isDefault) row["is_default"] = true;
            return row;
        };
        fixture.topics = [&](const Request &) {
            return Response{QJsonArray{topicDto(topicId, "Native topic", a2, true),
                topicDto(topicB, "Second topic", "bbbbbbbb-bbbb-4bbb-8bbb-0000000000b1", false)}};
        };
        fixture.counters = [&](const Request &request) -> Response {
            const auto id = request.target.path().section('/', -1);
            if (id == streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", QJsonValue(QJsonValue::Null)}}};
            return Response{topicDto(id, "Topic", QString(), id == topicId)};
        };
        fixture.messages = [&](const Request &request) -> Response {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            if (topic == topicB) return Response{QJsonArray{msgB}};
            return Response{QJsonArray{msgA2, msgA1}};
        };
        QVector<std::function<void()>> held;
        bool armDefer = false;
        const auto original = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (armDefer && request.method == "GET" && request.target.path().endsWith("/messages/")
                && QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId) {
                Response response{QJsonArray{msgA2, msgA1}};
                response.defer = [&](std::function<void()> send) { held.append(std::move(send)); };
                return response;
            }
            return original(request);
        };
        auto &c = fixture.controller;
        // The swap source here is the sweep-warmed slice of the DEFAULT topic
        // (in the aggregate view no topic is open, so the sweep treats it like
        // any unopened populated topic) - opt this case into the sweep.
        c.setTopicPreloadEnabledForTest(true);
        c.login(fixture.server.origin().toString(), "user", "password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId);
        QTRY_COMPARE(c.streams().size(), 1);
        c.selectStream(streamId);
        QTRY_COMPARE(c.messages().size(), 2); QTRY_VERIFY(!c.busy());
        QTRY_VERIFY(c.hasPreparedTopicSlice(topicId));
        c.selectTopic(topicB);
        QTRY_COMPARE(c.messages().size(), 1); QTRY_VERIFY(!c.busy());
        armDefer = true;
        c.selectTopic(topicId);
        // Swap-on-ready: A's rows are the first published frame, not the empty
        // window, while the deferred reload is still in flight.
        QVERIFY2(!c.messages().isEmpty(),
            qPrintable(QStringLiteral("blank frame on swap back: %1 rows published").arg(c.messages().size())));
        QTRY_COMPARE(held.size(), 1);
        QCOMPARE(c.messages().size(), 2); // the held reload has not landed yet
        held.first()();
        QTRY_VERIFY(!c.busy());
        QCOMPARE(c.messages().size(), 2);
        QTRY_VERIFY(c.hasPreparedTopicSlice(topicId));
    }

    void topicPreloadWarmsUnopenedTopicsWithBoundedTraffic()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString topicB = "bbbbbbbb-bbbb-4bbb-8bbb-00000000000b";
        const QString topicC = "cccccccc-cccc-4ccc-8ccc-00000000000c";
        const QString topicD = "dddddddd-dddd-4ddd-8ddd-00000000000d";
        const QString a1 = "aaaaaaaa-aaaa-4aaa-8aaa-0000000000c1";
        const QString a2 = "aaaaaaaa-aaaa-4aaa-8aaa-0000000000c2";
        const auto topicDto = [](const QString &id, const QString &name, const QString &last, bool isDefault) {
            QJsonObject row{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", name}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"updated_at", "2026-01-02T10:00:00Z"},
                {"last_message_uuid", last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(last)}};
            if (isDefault) row["is_default"] = true;
            return row;
        };
        fixture.topics = [&](const Request &) {
            return Response{QJsonArray{topicDto(topicId, "Native topic", a2, true),
                topicDto(topicB, "B", "bbbbbbbb-bbbb-4bbb-8bbb-0000000000c1", false),
                topicDto(topicC, "C", "cccccccc-cccc-4ccc-8ccc-0000000000c1", false),
                topicDto(topicD, "D", "dddddddd-dddd-4ddd-8ddd-0000000000d1", false)}};
        };
        fixture.counters = [&](const Request &request) -> Response {
            const auto id = request.target.path().section('/', -1);
            if (id == streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {""
                    "name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", QJsonValue(QJsonValue::Null)}}};
            return Response{topicDto(id, "Topic", QString(), id == topicId)};
        };
        fixture.messages = [&](const Request &request) -> Response {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            if (topic == topicD) return Response{QJsonObject{}, 500};
            if (topic == topicB)
                return Response{QJsonArray{message("B two", "bbbbbbbb-bbbb-4bbb-8bbb-0000000000c2", topicB),
                    message("B one", "bbbbbbbb-bbbb-4bbb-8bbb-0000000000c1", topicB)}};
            if (topic == topicC)
                return Response{QJsonArray{message("C one", "cccccccc-cccc-4ccc-8ccc-0000000000c1", topicC)}};
            return Response{QJsonArray{message("A two", a2, topicId), message("A one", a1, topicId)}};
        };
        QVector<QString> topicGets;
        const auto original = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path().endsWith("/messages/"))
                topicGets.append(QUrlQuery(request.target).queryItemValue("topic_uuid"));
            return original(request);
        };
        auto &c = fixture.controller;
        c.setTopicPreloadEnabledForTest(true);
        c.setTopicPreloadIntervalMsForTest(30);
        c.login(fixture.server.origin().toString(), "user", "password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId);
        QTRY_COMPARE(c.streams().size(), 1);
        c.selectStream(streamId);
        QTRY_COMPARE(c.messages().size(), 2); // topic A is the open one
        // B and C are never opened, yet the bounded sweep warms them.
        QTRY_VERIFY(c.hasPreparedTopicSlice(topicB));
        QTRY_VERIFY(c.hasPreparedTopicSlice(topicC));
        QTRY_VERIFY(!c.hasPreparedTopicSlice(topicD));
        QTest::qWait(400);
        QCOMPARE(topicGets.count(topicB), 1); // one page per topic, no re-requests
        QCOMPARE(topicGets.count(topicC), 1);
        QCOMPARE(topicGets.count(topicD), 1); // failing topic is skipped, not hammered
        // The prepared topic paints on the first frame of the route change.
        c.selectTopic(topicB);
        QVERIFY2(!c.messages().isEmpty(),
            "a prepared topic must show its own rows on the first visible frame");
    }

    // Issue #718 review round (exo-review findings 1/4): a catalog larger than
    // the 512-slice budget must still request each topic exactly once. Eviction
    // from the bounded warm set may not re-arm a topic as a sweep candidate -
    // the done ledger is tracked independently of the bounded slice cache.
    void topicPreloadSweepPastSliceBudgetRequestsEachTopicOnce()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const int sweepCount = 520; // the warm-slice budget is 512
        const auto aMsg = message("A one", "aaaaaaaa-aaaa-4aaa-8aaa-0000000000d1");
        const auto topicDto = [](const QString &id, const QString &name,
                                 const QString &last, bool isDefault) {
            QJsonObject row{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", name}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"updated_at", "2026-01-02T10:00:00Z"},
                {"last_message_uuid", last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(last)}};
            if (isDefault) row["is_default"] = true;
            return row;
        };
        const auto hex12 = [](int i) { return QString("%1").arg(i, 12, 16, QLatin1Char('0')); };
        QHash<QString, QJsonObject> swept; // topic uuid -> its single message row
        QJsonArray catalog{topicDto(topicId, "Native topic", aMsg.value("uuid").toString(), true)};
        for (int i = 0; i < sweepCount; ++i) {
            const auto topicUuid = QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-") + hex12(i);
            const auto msg = message(QStringLiteral("M%1").arg(i),
                QStringLiteral("cccccccc-cccc-4ccc-8ccc-") + hex12(i), topicUuid);
            swept.insert(topicUuid, msg);
            catalog.append(topicDto(topicUuid, QStringLiteral("T%1").arg(i),
                msg.value("uuid").toString(), false));
        }
        fixture.topics = [&](const Request &) { return Response{catalog}; };
        fixture.counters = [&](const Request &request) -> Response {
            const auto id = request.target.path().section('/', -1);
            if (id == streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", QJsonValue(QJsonValue::Null)}}};
            for (const auto &value : catalog)
                if (value.toObject().value("uuid").toString() == id)
                    return Response{value.toObject()};
            return Response{topicDto(id, "Topic", QString(), false)};
        };
        fixture.messages = [&](const Request &request) -> Response {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            if (topic == topicId || topic.isEmpty()) return Response{QJsonArray{aMsg}};
            const auto found = swept.constFind(topic);
            if (found != swept.constEnd()) return Response{QJsonArray{*found}};
            return Response{QJsonArray{}};
        };
        QVector<QString> sweepGets;
        const auto original = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path().endsWith("/messages/")) {
                const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
                if (!topic.isEmpty() && topic != topicId) sweepGets.append(topic);
            }
            return original(request);
        };
        auto &c = fixture.controller;
        c.setTopicPreloadEnabledForTest(true);
        c.setTopicPreloadIntervalMsForTest(0);
        c.login(fixture.server.origin().toString(), "user", "password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId);
        QTRY_COMPARE(c.streams().size(), 1);
        c.selectStream(streamId);
        for (int i = 0; i < 600 && sweepGets.size() < sweepCount; ++i) QTest::qWait(50);
        QVERIFY2(sweepGets.size() >= sweepCount,
            qPrintable(QStringLiteral("sweep stalled at %1 requests").arg(sweepGets.size())));
        // The sweep must settle: past the budget the done ledger (not the
        // bounded slice map) decides candidacy, so the request count freezes.
        QTest::qWait(600);
        const int total = sweepGets.size();
        QCOMPARE(total, sweepCount);
        const QSet<QString> distinct(sweepGets.begin(), sweepGets.end());
        const int distinctCount = distinct.size();
        QCOMPARE(distinctCount, sweepCount);
    }

    // Issue #718 review round (exo-review finding 2): a transient preload
    // failure skips the topic only until the next verified catalog - the next
    // catalog commit must retry it, while the failing topic is not hammered.
    void topicPreloadTransientFailureRetriesOnNextVerifiedCatalog()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString topicF = "ffffffff-ffff-4fff-8fff-00000000000f";
        const QString topicOther = "9f9f9f9f-9f9f-4f9f-8f9f-9f9f9f9f9f9f";
        const QString otherStream = "9e9e9e9e-9e9e-4e9e-8e9e-9e9e9e9e9e9e";
        const auto aMsg = message("A one", "aaaaaaaa-aaaa-4aaa-8aaa-0000000000e1");
        const auto fMsg = message("F one", "ffffffff-ffff-4fff-8fff-0000000000e1", topicF);
        const auto streamDto = [](const QString &id) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId},
                {"name", id == streamId ? QStringLiteral("Main") : QStringLiteral("Other")},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"updated_at", "2026-01-02T10:00:00Z"},
                {"last_message_uuid", QJsonValue(QJsonValue::Null)}};
        };
        const auto topicDto = [](const QString &stream, const QString &id,
                                 const QString &last, bool isDefault) {
            QJsonObject row{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream},
                {"name", QStringLiteral("Topic")}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"updated_at", "2026-01-02T10:00:00Z"},
                {"last_message_uuid", last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(last)}};
            if (isDefault) row["is_default"] = true;
            return row;
        };
        fixture.streams = [&](const Request &) {
            return Response{QJsonArray{streamDto(streamId), streamDto(otherStream)}};
        };
        fixture.topics = [&](const Request &request) -> Response {
            const auto parent = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (parent == otherStream)
                return Response{QJsonArray{topicDto(otherStream, topicOther, QString(), true)}};
            return Response{QJsonArray{
                topicDto(streamId, topicId, aMsg.value("uuid").toString(), true),
                topicDto(streamId, topicF, fMsg.value("uuid").toString(), false)}};
        };
        fixture.counters = [&](const Request &request) -> Response {
            const auto id = request.target.path().section('/', -1);
            if (id == streamId) return Response{streamDto(streamId)};
            if (id == otherStream) return Response{streamDto(otherStream)};
            if (id == topicF) return Response{topicDto(streamId, topicF, fMsg.value("uuid").toString(), false)};
            if (id == topicId) return Response{topicDto(streamId, topicId, aMsg.value("uuid").toString(), true)};
            return Response{topicDto(otherStream, id, QString(), true)};
        };
        int fAttempts = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const QUrlQuery query(request.target);
            const auto topic = query.queryItemValue("topic_uuid");
            if (topic == topicF) {
                ++fAttempts;
                if (fAttempts == 1) return Response{QJsonObject{{"detail", "transient"}}, 500};
                return Response{QJsonArray{fMsg}};
            }
            if (query.queryItemValue("stream_uuid") == otherStream) return Response{QJsonArray{}};
            return Response{QJsonArray{aMsg}};
        };
        auto &c = fixture.controller;
        c.setTopicPreloadEnabledForTest(true);
        c.setTopicPreloadIntervalMsForTest(0);
        c.login(fixture.server.origin().toString(), "user", "password", "");
        QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId);
        QTRY_COMPARE(c.streams().size(), 2);
        c.selectStream(streamId);
        QTRY_VERIFY(!c.busy());
        QTRY_COMPARE(fAttempts, 1); // attempted once...
        QTest::qWait(400);
        QCOMPARE(fAttempts, 1);     // ...skipped, not hammered
        QVERIFY(!c.hasPreparedTopicSlice(topicF));
        // The next verified catalog (a re-select across streams) must retry.
        c.selectStream(otherStream);
        QTRY_VERIFY(!c.busy());
        c.selectStream(streamId);
        QTRY_VERIFY(!c.busy());
        QTRY_VERIFY(c.hasPreparedTopicSlice(topicF));
        QCOMPARE(fAttempts, 2);
    }
};

QTEST_MAIN(ControllerSidebarTest)
#include "native-controller-sidebar-test.moc"
