#include "support/native-test-harness.h"
#include "support/controller-poll-fixtures.h"

class ControllerForwardTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Issue #7 multi-forward (docs/NATIVE_DESKTOP_MULTI_FORWARD.md) ----

    static QJsonObject forwardEchoDto(const QJsonObject &posted, const QString &publicId)
    {
        auto dto = message(posted.value("payload").toObject().value("content").toString(), publicId);
        dto.insert("stream_uuid", posted.value("stream_uuid").toString());
        dto.insert("topic_uuid", posted.value("topic_uuid").toString());
        return dto;
    }

    // Review round 2 (cid 4586649206): the forward destination is resolved
    // against the stream catalog, so a test that forwards into a dedicated
    // target channel must ship that channel in the catalog alongside the
    // default unencrypted stream.
    static void catalogTargetStream(Fixture &fixture, const QString &targetStream)
    {
        fixture.streams = [&](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}},
                QJsonObject{{"uuid", targetStream}, {"project_id", projectId},
                    {"name", "Target channel"}, {"encryption", false}}}};
        };
    }

    void forwardMessagesCollapsesDuplicateSourcesInOneBatch()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString sourceB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        catalogTargetStream(fixture, targetStream);
        QStringList forwardUuidParams;
        int posts = 0;
        QJsonObject postedBody;
        QString echoUuid;
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (request.method == "GET" && query.hasQueryItem(QStringLiteral("uuid"))) {
                QStringList uuids;
                const auto items = query.queryItems();
                for (const auto &item : items)
                    if (item.first == QLatin1String("uuid")) uuids << item.second;
                forwardUuidParams = uuids;
                QJsonArray rows;
                for (const auto &uuid : uuids) {
                    if (uuid == sourceA) rows.append(message("First source", sourceA));
                    if (uuid == sourceB) rows.append(message("Second source", sourceB));
                }
                return Response{rows};
            }
            if (request.method == "POST") {
                ++posts;
                postedBody = request.body;
                echoUuid = QUuid::createUuidV5(QUuid(postedBody.value("topic_uuid").toString()),
                    postedBody.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                return Response{forwardEchoDto(postedBody, echoUuid)};
            }
            if (request.method == "GET" && !echoUuid.isEmpty()
                && request.target.path() == "/api/workspace/v1/messenger/messages/" + echoUuid)
                return Response{forwardEchoDto(postedBody, echoUuid)};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start({}, 2));
        QVERIFY(fixture.controller.canForwardMessages());
        // One source repeated, one blank entry and one fresh source: the batch
        // must forward each distinct source exactly once (uniqueForwardMessageUuids).
        fixture.controller.forwardMessages({sourceA, QStringLiteral(" %1 ").arg(sourceA), QString(), sourceB},
            targetStream, targetTopic);
        QTRY_VERIFY_WITH_TIMEOUT(posts == 1, 3000);
        QCOMPARE(forwardUuidParams, QStringList({sourceA, sourceB}));
        QCOMPARE(postedBody.value("stream_uuid").toString(), targetStream);
        QCOMPARE(postedBody.value("topic_uuid").toString(), targetTopic);
        const auto payload = postedBody.value("payload").toObject();
        QCOMPARE(payload.value("kind").toString(), QStringLiteral("markdown"));
        const auto wire = payload.value("content").toString();
        QCOMPARE(wire.count(QStringLiteral("urn:forward:") + sourceA), 1);
        QCOMPARE(wire.count(QStringLiteral("urn:forward:") + sourceB), 1);
        QCOMPARE(wire.count(QStringLiteral("\n\n")), 1);
        QVERIFY(isUuid(postedBody.value("uuid").toString()));
        // Confirmation is the GET-only read-back; the POST is never repeated.
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardPending(), 3000);
        QVERIFY(fixture.controller.error().isEmpty());
        QCOMPARE(posts, 1);
    }

    void forwardConfirmationCheckNeverResendsThePost()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        int posts = 0;
        int checkGets = 0;
        catalogTargetStream(fixture, targetStream);
        bool serverKept = false;
        QJsonObject postedBody;
        QString echoUuid;
        QByteArray checkAuthorization;
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (request.method == "GET" && query.hasQueryItem(QStringLiteral("uuid"))) {
                return Response{QJsonArray{message("First source", sourceA)}};
            }
            if (request.method == "POST") {
                ++posts;
                postedBody = request.body;
                echoUuid = QUuid::createUuidV5(QUuid(postedBody.value("topic_uuid").toString()),
                    postedBody.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                if (!serverKept) return Response{QJsonObject{{QStringLiteral("detail"), QStringLiteral("boom")}}, 500, 0, {}};
                return Response{forwardEchoDto(postedBody, echoUuid)};
            }
            if (request.method == "GET" && !echoUuid.isEmpty()
                && request.target.path() == "/api/workspace/v1/messenger/messages/" + echoUuid) {
                ++checkGets;
                checkAuthorization = request.headers.value("authorization");
                if (serverKept) return Response{forwardEchoDto(postedBody, echoUuid)};
                return Response{QJsonObject{}, 404, 0, {}};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start({}, 2));
        fixture.controller.forwardMessages({sourceA, sourceA}, targetStream, targetTopic);
        // The ambiguous POST stays unconfirmed and is never repeated.
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.forwardPending()
            && fixture.controller.error().contains("Forwarding was not confirmed"), 3000);
        QCOMPARE(posts, 1);
        // A Check against a server that never kept the message stays unresolved.
        fixture.controller.checkForwardStatus();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.error().contains("could not be verified"), 3000);
        QVERIFY(fixture.controller.forwardPending());
        QCOMPARE(posts, 1);
        // When the server actually committed it (the 5xx lied), Check confirms
        // with the GET read-back alone - still exactly one POST.
        fixture.api.setAccessToken("renewed-member-token", true);
        serverKept = true;
        fixture.controller.checkForwardStatus();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardPending(), 3000);
        QVERIFY(fixture.controller.error().isEmpty());
        QCOMPARE(posts, 1);
        QCOMPARE(checkGets, 2);
        QCOMPARE(checkAuthorization, QByteArray("Bearer renewed-member-token"));
    }

    void sessionChangeSettlesActiveForward_data()
    {
        QTest::addColumn<bool>("holdPost");
        QTest::addColumn<bool>("sameOwner");
        QTest::newRow("renew-during-source-read") << false << true;
        QTest::newRow("renew-during-post") << true << true;
        QTest::newRow("replace-session-during-source-read") << false << false;
        QTest::newRow("replace-session-during-post") << true << false;
    }

    void sessionChangeSettlesActiveForward()
    {
        QFETCH(bool, holdPost);
        QFETCH(bool, sameOwner);
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString source = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        catalogTargetStream(fixture, targetStream);
        std::function<void()> release;
        int posts = 0, checks = 0;
        QString publicId;
        QJsonObject posted;
        fixture.messages = [&](const Request &request) {
            const bool sourceRead = request.method == "GET" && QUrlQuery(request.target).hasQueryItem("uuid");
            Response response{QJsonArray{}};
            if (sourceRead) {
                response.data = QJsonArray{message("Forward source", source)};
            } else if (request.method == "POST") {
                ++posts;
                posted = request.body;
                publicId = QUuid::createUuidV5(QUuid(targetTopic), posted.value("uuid").toString().toUtf8())
                    .toString(QUuid::WithoutBraces);
                response.data = forwardEchoDto(posted, publicId);
            } else if (!publicId.isEmpty() && request.target.path().endsWith("/" + publicId)) {
                ++checks;
                response.data = forwardEchoDto(posted, publicId);
            }
            if ((sourceRead && !holdPost) || (request.method == "POST" && holdPost))
                response.defer = [&](std::function<void()> deliver) { release = std::move(deliver); };
            return response;
        };
        QVERIFY(fixture.start({}, 2));
        fixture.controller.forwardMessages({source}, targetStream, targetTopic);
        QTRY_VERIFY_WITH_TIMEOUT(static_cast<bool>(release), 3000);
        QVERIFY(fixture.controller.forwardPending());
        QVERIFY(!fixture.controller.forwardAwaitingCheck());
        fixture.api.setAccessToken("replacement-member-token", sameOwner);
        QCOMPARE(fixture.controller.forwardAwaitingCheck(), sameOwner && holdPost);
        QCOMPARE(fixture.controller.forwardPending(), sameOwner && holdPost);
        QVERIFY(!fixture.controller.forwardError().isEmpty());
        release();
        fixture.controller.checkForwardStatus();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardPending(), 3000);
        QCOMPARE(posts, holdPost ? 1 : 0);
        QCOMPARE(checks, sameOwner && holdPost ? 1 : 0);
        QVERIFY(fixture.controller.canForwardMessages());
    }

    void forwardMessagesRejectsEmptyAndInvalidBatches()
    {
        Fixture fixture;
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        QVERIFY(fixture.start());
        fixture.controller.forwardMessages({QStringLiteral(""), QStringLiteral("  ")},
            targetStream, secondTopicId);
        QTest::qWait(200);
        QVERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.forwardPending());
        fixture.controller.forwardMessages({sourceA}, QStringLiteral("not-a-uuid"), secondTopicId);
        QTest::qWait(200);
        QVERIFY(!fixture.controller.forwardPending());
        for (const auto &request : fixture.server.requests)
            QVERIFY(!(request.method == "POST" && request.target.path().endsWith("/messages/")));
    }

    // The forward modal drives these reactive states (issue #7 slice 2):
    // busy while a batch is in flight, unconfirmed with a Check escape after an
    // ambiguous POST, and a clean cleared state once the GET read-back confirms.
    void forwardStatePropertiesFollowTheBatch()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        catalogTargetStream(fixture, targetStream);
        int posts = 0;
        bool serverKept = false;
        QJsonObject postedBody;
        QString echoUuid;
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (request.method == "GET" && query.hasQueryItem(QStringLiteral("uuid")))
                return Response{QJsonArray{message("First source", sourceA)}};
            if (request.method == "POST") {
                ++posts;
                postedBody = request.body;
                echoUuid = QUuid::createUuidV5(QUuid(postedBody.value("topic_uuid").toString()),
                    postedBody.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                if (!serverKept) return Response{QJsonObject{{QStringLiteral("detail"), QStringLiteral("boom")}}, 500, 0, {}};
                return Response{forwardEchoDto(postedBody, echoUuid)};
            }
            if (request.method == "GET" && !echoUuid.isEmpty()
                && request.target.path() == "/api/workspace/v1/messenger/messages/" + echoUuid) {
                if (serverKept) return Response{forwardEchoDto(postedBody, echoUuid)};
                return Response{QJsonObject{}, 404, 0, {}};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start({}, 2));
        QVERIFY(!fixture.controller.forwardPending());
        QVERIFY(!fixture.controller.forwardAwaitingCheck());
        QVERIFY(fixture.controller.forwardError().isEmpty());
        fixture.controller.forwardMessages({sourceA}, targetStream, targetTopic);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.forwardPending()
            && fixture.controller.forwardAwaitingCheck(), 3000);
        QVERIFY(fixture.controller.forwardError().contains("Forwarding was not confirmed"));
        // A Check that cannot verify the batch keeps the modal in the same
        // unconfirmed state, with a fresh read-back error and no second POST.
        fixture.controller.checkForwardStatus();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.forwardError().contains("could not be verified"), 3000);
        QVERIFY(fixture.controller.forwardPending());
        QVERIFY(fixture.controller.forwardAwaitingCheck());
        QCOMPARE(posts, 1);
        // The confirming Check lands the batch and resets all three properties.
        serverKept = true;
        fixture.controller.checkForwardStatus();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardPending(), 3000);
        QVERIFY(!fixture.controller.forwardAwaitingCheck());
        QVERIFY(fixture.controller.forwardError().isEmpty());
        QCOMPARE(posts, 1);
    }

    // The picker lists one arbitrary stream's topics (reference forward modal);
    // the options load GET-only and are served from cache afterwards.
    void fetchForwardTopicsServesForwardPicker()
    {
        Fixture fixture;
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        int pickerGets = 0;
        fixture.topics = [&](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == targetStream) {
                ++pickerGets;
                return Response{QJsonArray{QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId},
                    {"stream_uuid", targetStream}, {"name", "Picker topic"}}}};
            }
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.forwardTopicOptions(targetStream).isEmpty());
        fixture.controller.fetchForwardTopics(targetStream);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardTopicsPending(targetStream), 3000);
        const auto rows = fixture.controller.forwardTopicOptions(targetStream);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap().value("id").toString(), secondTopicId);
        QCOMPARE(rows[0].toMap().value("name").toString(), QStringLiteral("Picker topic"));
        QCOMPARE(pickerGets, 1);
        // The repeat request is served from the cache - no second GET - and an
        // invalid stream uuid never reaches the network.
        fixture.controller.fetchForwardTopics(targetStream);
        QTest::qWait(200);
        QCOMPARE(pickerGets, 1);
        fixture.controller.fetchForwardTopics(QStringLiteral("not-a-uuid"));
        QTest::qWait(100);
        QCOMPARE(pickerGets, 1);
    }

    // Review round 1, finding 1 (cid 4586012787): the reference forward submit
    // path refuses an encrypted destination before any POST - the sole send
    // route sendMessengerMessage throws on stream.encryption === true
    // (workspace_ui 75bd21f37 entities/messenger/messenger-message-actions.lib.ts:309-311).
    // The native forward path POSTs plaintext markdown with no crypto route,
    // so a direct forwardMessages call against an encrypted stream must be
    // rejected without any POST.
    void forwardMessagesRefusesEncryptedDestinationWithoutPost()
    {
        Fixture fixture;
        const QString encryptedStream = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        int posts = 0;
        fixture.streams = [&](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}},
                QJsonObject{{"uuid", encryptedStream}, {"project_id", projectId},
                    {"name", "Encrypted channel"}, {"encryption", true}}}};
        };
        QJsonObject postedBody;
        QString echoUuid;
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (request.method == "GET" && query.hasQueryItem(QStringLiteral("uuid")))
                return Response{QJsonArray{message("First source", sourceA)}};
            if (request.method == "POST") {
                ++posts;
                postedBody = request.body;
                echoUuid = QUuid::createUuidV5(QUuid(postedBody.value("topic_uuid").toString()),
                    postedBody.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                auto dto = message(postedBody.value("payload").toObject().value("content").toString(), echoUuid);
                dto.insert("stream_uuid", postedBody.value("stream_uuid").toString());
                dto.insert("topic_uuid", postedBody.value("topic_uuid").toString());
                return Response{dto};
            }
            return Response{QJsonArray{}};
        };
        // The fixture start helper waits for a single-stream catalog, so the
        // two-stream scene is booted with the same steps inline.
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user",
            "synthetic-password", "");
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.projects().size() == 1, 2000);
        fixture.controller.openProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.streams().size() == 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.busy(), 2000);
        QVERIFY(fixture.controller.canForwardMessages());
        fixture.controller.forwardMessages({sourceA}, encryptedStream, targetTopic);
        QTest::qWait(300);
        // The encrypted destination is refused outright: no batch identity is
        // retained, nothing is posted, and the reason reaches the forward
        // alert channel the modal shows.
        QCOMPARE(posts, 0);
        QVERIFY(!fixture.controller.forwardPending());
        QVERIFY(fixture.controller.forwardError().contains("encrypted"));
        QVERIFY(fixture.controller.error().contains("encrypted"));
    }

    // Review round 2 (cid 4586649206): the encrypted guard may not be a scan
    // for a *matching* encrypted row - a destination stream uuid that is
    // absent from the loaded stream catalog (stale or incomplete catalog, or
    // a direct controller call) cannot be proven unencrypted, so it must be
    // refused with a visible error before any source fetch or POST.
    void forwardMessagesRefusesDestinationAbsentFromStreamCatalog()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString encryptedStream = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
        const QString unknownStream = "ffffffff-ffff-4fff-8fff-ffffffffffff";
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        int posts = 0;
        int sourceGets = 0;
        QJsonObject postedBody;
        QString echoUuid;
        fixture.streams = [&](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}},
                QJsonObject{{"uuid", encryptedStream}, {"project_id", projectId},
                    {"name", "Encrypted channel"}, {"encryption", true}}}};
        };
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (request.method == "GET" && query.hasQueryItem(QStringLiteral("uuid"))) {
                ++sourceGets;
                return Response{QJsonArray{message("First source", sourceA)}};
            }
            if (request.method == "POST") {
                ++posts;
                postedBody = request.body;
                echoUuid = QUuid::createUuidV5(QUuid(postedBody.value("topic_uuid").toString()),
                    postedBody.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                return Response{forwardEchoDto(postedBody, echoUuid)};
            }
            if (request.method == "GET" && !echoUuid.isEmpty()
                && request.target.path() == "/api/workspace/v1/messenger/messages/" + echoUuid)
                return Response{forwardEchoDto(postedBody, echoUuid)};
            return Response{QJsonArray{}};
        };
        // The fixture start helper waits for a single-stream catalog, so the
        // two-stream scene is booted with the same steps inline.
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user",
            "synthetic-password", "");
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.projects().size() == 1, 2000);
        fixture.controller.openProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.streams().size() == 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.busy(), 2000);
        QVERIFY(fixture.controller.canForwardMessages());
        // A destination that is not in the catalog cannot be proven to be an
        // unencrypted chat: it is refused outright - no source fetch, no POST,
        // and the reason reaches the forward alert channel the modal shows.
        fixture.controller.forwardMessages({sourceA}, unknownStream, targetTopic);
        QTest::qWait(300);
        QCOMPARE(posts, 0);
        QCOMPARE(sourceGets, 0);
        QVERIFY(!fixture.controller.forwardPending());
        QVERIFY(fixture.controller.forwardError().contains("unknown destination"));
        QVERIFY(fixture.controller.error().contains("unknown destination"));
        // The round 1 guard for a known encrypted stream is still in force.
        fixture.controller.forwardMessages({sourceA}, encryptedStream, targetTopic);
        QTest::qWait(300);
        QCOMPARE(posts, 0);
        QVERIFY(fixture.controller.forwardError().contains("encrypted"));
        // A known unencrypted catalog destination still forwards normally.
        fixture.controller.forwardMessages({sourceA}, streamId, targetTopic);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardPending(), 3000);
        QCOMPARE(posts, 1);
        QVERIFY(fixture.controller.error().isEmpty());
    }

    // Review round 1, finding 2 (cid 4586012796): an unconfirmed (pending)
    // forward must block a new batch (a second begin would replace the
    // retained identity and strand the first ambiguous POST without its
    // Check escape) while the pending batch stays Checkable.
    void forwardPendingBatchBlocksSecondForwardAndStaysCheckable()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString sourceB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        catalogTargetStream(fixture, targetStream);
        int posts = 0;
        bool serverKept = false;
        QJsonObject postedBody;
        QString echoUuid;
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (request.method == "GET" && query.hasQueryItem(QStringLiteral("uuid"))) {
                QJsonArray rows;
                if (query.queryItemValue(QStringLiteral("uuid")) == sourceA)
                    rows.append(message("First source", sourceA));
                if (query.queryItemValue(QStringLiteral("uuid")) == sourceB)
                    rows.append(message("Second source", sourceB));
                return Response{rows};
            }
            if (request.method == "POST") {
                ++posts;
                postedBody = request.body;
                echoUuid = QUuid::createUuidV5(QUuid(postedBody.value("topic_uuid").toString()),
                    postedBody.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                if (!serverKept) return Response{QJsonObject{{QStringLiteral("detail"), QStringLiteral("boom")}}, 500, 0, {}};
                return Response{forwardEchoDto(postedBody, echoUuid)};
            }
            if (request.method == "GET" && !echoUuid.isEmpty()
                && request.target.path() == "/api/workspace/v1/messenger/messages/" + echoUuid)
                return serverKept ? Response{forwardEchoDto(postedBody, echoUuid)}
                                  : Response{QJsonObject{}, 404, 0, {}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start({}, 2));
        fixture.controller.forwardMessages({sourceA}, targetStream, targetTopic);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.forwardPending()
            && fixture.controller.forwardAwaitingCheck(), 3000);
        QCOMPARE(posts, 1);
        // While the batch is unconfirmed-pending the controller refuses a new
        // forward: canForwardMessages is false and the second begin is a no-op.
        QVERIFY(!fixture.controller.canForwardMessages());
        fixture.controller.forwardMessages({sourceB}, targetStream, targetTopic);
        QTest::qWait(300);
        QCOMPARE(posts, 1);
        QVERIFY(fixture.controller.forwardPending());
        // The first batch is still the retained one and Check still confirms
        // it through the GET read-back alone.
        serverKept = true;
        fixture.controller.checkForwardStatus();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardPending(), 3000);
        QVERIFY(fixture.controller.error().isEmpty());
        QCOMPARE(posts, 1);
        // Once confirmed, a new forward is admissible again.
        QVERIFY(fixture.controller.canForwardMessages());
    }

    // Review round 1, finding 3 (cid 4586012826): a picker reopen (force
    // refresh) must re-read the topic list so topics created mid-session show
    // up, even when the previous fetch was empty or single-topic.
    void fetchForwardTopicsForceRefreshShowsNewTopics()
    {
        Fixture fixture;
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        bool includeExtra = false;
        int pickerGets = 0;
        fixture.topics = [&](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream != targetStream)
                return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Native topic"}}}};
            ++pickerGets;
            QJsonArray rows{QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId},
                {"stream_uuid", targetStream}, {"name", "First topic"}}};
            if (includeExtra)
                rows.append(QJsonObject{{"uuid", "f5f5f5f5-f5f5-4f5f-8f5f-f5f5f5f5f5f5"},
                    {"project_id", projectId}, {"stream_uuid", targetStream}, {"name", "Added topic"}});
            return Response{rows};
        };
        QVERIFY(fixture.start());
        fixture.controller.fetchForwardTopics(targetStream);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.forwardTopicsPending(targetStream), 3000);
        QCOMPARE(fixture.controller.forwardTopicOptions(targetStream).size(), 1);
        QCOMPARE(pickerGets, 1);
        // A topic appears mid-session and the picker reopens on the same
        // stream: the force refresh re-reads the list instead of trusting the
        // stale cache, while the plain cached read stays GET-free.
        includeExtra = true;
        fixture.controller.fetchForwardTopics(targetStream);
        QTest::qWait(200);
        QCOMPARE(pickerGets, 1);
        // The QML picker invokes the two-argument refresh on every stream
        // activation; before the fix no such overload exists and nothing
        // refreshes.
        QMetaObject::invokeMethod(&fixture.controller, "fetchForwardTopics",
            Q_ARG(QString, targetStream), Q_ARG(bool, true));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.forwardTopicOptions(targetStream).size() == 2, 3000);
        QCOMPARE(pickerGets, 2);
    }

    // Review round 1, finding 4 (cid 4586012837): the reference keeps every
    // server message selectable (workspace_ui 75bd21f37
    // widgets/workspace-message-list/workspace-message-list.ui.tsx:503) and
    // rejects the whole batch with a visible error when any source cannot
    // build a snapshot (workspace-forward-message.lib.ts:78-84 + ui.tsx:449-451).
    // The native path must do the same instead of silently dropping the
    // unsupported source and posting the rest.
    void forwardMessagesRejectsBatchWithUnsupportedSources()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString sourceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString sourceB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        const QString targetStream = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        const QString targetTopic = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        catalogTargetStream(fixture, targetStream);
        int posts = 0;
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (request.method == "GET" && query.hasQueryItem(QStringLiteral("uuid"))) {
                QJsonArray rows;
                for (const auto &item : query.queryItems()) {
                    if (item.first != QLatin1String("uuid")) continue;
                    if (item.second == sourceA) rows.append(message("First source", sourceA));
                    if (item.second == sourceB) {
                        auto encrypted = message({}, sourceB);
                        encrypted["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", sourceB}, {"content", "opaque"}};
                        rows.append(encrypted);
                    }
                }
                return Response{rows};
            }
            if (request.method == "POST") {
                ++posts;
                return Response{forwardEchoDto(request.body,
                    QUuid::createUuidV5(QUuid(request.body.value("topic_uuid").toString()),
                        request.body.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces))};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start({}, 2));
        fixture.controller.forwardMessages({sourceA, sourceB}, targetStream, targetTopic);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.error().contains("Only text"), 3000);
        // The mixed batch never reaches the network: no snapshot without the
        // unsupported source, and no silent partial forward either.
        QTest::qWait(300);
        QCOMPARE(posts, 0);
        QVERIFY(!fixture.controller.forwardPending());
    }
};

QTEST_MAIN(ControllerForwardTest)
#include "native-controller-forward-test.moc"
