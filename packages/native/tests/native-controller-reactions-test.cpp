#include "support/native-test-harness.h"
#include "support/controller-realtime-fixtures.h"

class ControllerReactionsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void delayedMessageMutationCannotEraseConfirmedReactions()
    {
        Fixture fixture; int gets = 0; bool applied = false;
        auto row = message(); row["read"] = false; row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/star/invoke")) { row["starred"] = true; return Response{row}; }
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            return Response{row, 200, ++gets == 1 ? 350 : 0};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                applied = true; row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
                return Response{fact, 201};
            }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.starMessage(messageId); QTRY_COMPARE(gets, 1);
        fixture.controller.toggleReaction(messageId, "heart"); QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read", QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{messageId}}})));
        // Server sockets can close before the client delivers its canonical
        // read-back. Observe confirmation before checking preserved fields.
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("starred").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("reactions").toList().size(), 1);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactions").toList().first().toMap().value("own").toBool());
        QVERIFY(modelMessage(fixture.controller, messageId).value("starred").toBool());
        QVERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
    }

    void reactionNotificationCanDestroyControllerBeforeAnyRequest()
    {
        Fixture fixture;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        fixture.messages = [&](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        auto *controller = new NativeController(&fixture.api, &fixture.session);
        QPointer<NativeController> guard(controller);
        controller->refresh(); QTRY_COMPARE(controller->streams().size(), 1);
        controller->selectStream(streamId); QTRY_COMPARE(controller->topics().size(), 1);
        controller->selectTopic(topicId); QTRY_VERIFY(!controller->busy());
        connect(controller, &NativeController::messagesChanged, controller, [controller] { delete controller; });
        controller->toggleReaction(messageId, "heart");
        QVERIFY(guard.isNull());
        QVERIFY(std::none_of(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &request) {
            return request.target.path().contains("/message_reactions/");
        }));
    }

    void oldReactionRetryTimerCannotJoinRenewedGeneration()
    {
        Fixture fixture; bool rotate = false, applied = false; int confirmations = 0, mutations = 0;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.authentication = [&](const Request &request) {
            if (rotate && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-rotated-reaction-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            ++confirmations; return Response{row, 200, applied ? 600 : 0};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{fact, 201}; }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_COMPARE(confirmations, 2);
        QSignalSpy authenticated(&fixture.session, &Session::authenticated);
        rotate = applied = true;
        row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        fixture.session.requestRefresh(); QTRY_COMPARE(authenticated.size(), 1); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.toggleReaction(messageId, "heart"); QTRY_COMPARE(confirmations, 3);
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(confirmations, 3); QCOMPARE(mutations, 1);
    }

    void localSendSignalRequiresVerifiedCurrentConversation_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"current", "other-route", "ambiguous", "unverified"}) QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void localSendSignalRequiresVerifiedCurrentConversation()
    {
        QFETCH(QString, scenario);
        Fixture fixture; fixture.extraTopic = true; bool gotCanonical = false;
        auto saved = message("sent text", returnedId);
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") return scenario == "ambiguous" ? Response{QJsonObject{}, 503} : Response{saved};
            if (request.target.path().endsWith(returnedId)) {
                gotCanonical = true; auto value = saved; if (scenario == "unverified") value["is_own"] = false;
                return Response{value, 200, 100};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QSignalSpy sent(&fixture.controller, &NativeController::messageSent);
        fixture.controller.setDraft("sent text"); fixture.controller.send();
        if (scenario != "ambiguous") QTRY_VERIFY(gotCanonical);
        if (scenario == "other-route") fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(sent.size(), scenario == "current" ? 1 : 0);
        if (!sent.isEmpty()) { QCOMPARE(sent.first().at(0).toString(), returnedId); QCOMPARE(sent.first().at(1).toString(), streamId); QCOMPARE(sent.first().at(2).toString(), topicId); }
    }

    void websocketMovedReactionRefreshesBothLoadedMessages()
    {
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        QVector<QJsonObject> events;
        QMap<QString, QJsonObject> rows{{messageId, message("old placement")}, {returnedId, message("new placement", returnedId)}};
        for (auto &row : rows) { row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{}; }
        QSet<QString> fetched;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{rows.value(messageId), rows.value(returnedId)}};
            const auto id = request.target.path().section('/', -1); fetched.insert(id); return Response{rows.value(id)};
        };
        fixture.realtime = [&](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray result;
            for (const auto &event : events)
                if (event.value("epoch_version").toInt() > QUrlQuery(request.target).queryItemValue("epoch_version>").toInt()) result.append(event);
            return Response{result};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session); fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        rows[returnedId]["reactions"] = QJsonObject{{"heart", 1}};
        rows[returnedId]["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        const auto event = controllerEvent("message_reaction.updated", QJsonObject{{"uuid", topicId}, {"project_id", projectId},
            {"message_uuid", returnedId}, {"user_uuid", userId}, {"emoji_name", "heart"}, {"source_name", "zulip"},
            {"old_message_uuid", messageId}, {"old_emoji_name", "future-provider-emoji"}, {"old_source_name", "zulip"}});
        events.append(event);
        gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(12));
        QTRY_COMPARE(fetched.size(), 2);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("reactions").toList().size(), 1);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactions").toList().isEmpty());
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(), "new placement");
    }

    void existingProviderAliasKeepsExactOwnedReactionIdentity()
    {
        Fixture fixture; QByteArray method;
        auto row = message(); row["reactions"] = QJsonObject{{"like", 1}}; row["reaction_users"] = QJsonObject{{"like", QJsonArray{userId}}};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "like"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method != "GET") { method = request.method; return Response{QJsonObject{}, 403}; }
            return Response{QJsonArray{fact}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "like");
        QTRY_VERIFY(!method.isEmpty()); QCOMPARE(method, QByteArray("DELETE"));
    }

    void reactionPreflightRejectsForeignOwnershipAndBadPagination_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"foreign-user", "wrong-project", "wrong-message", "duplicate-emoji", "loop-marker", "forbidden"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void reactionPreflightRejectsForeignOwnershipAndBadPagination()
    {
        QFETCH(QString, scenario);
        Fixture fixture; int mutations = 0, reads = 0;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        auto fact = QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        if (scenario == "foreign-user") fact["user_uuid"] = streamId;
        if (scenario == "wrong-project") fact["project_id"] = userId;
        if (scenario == "wrong-message") fact["message_uuid"] = returnedId;
        fixture.messages = [&](const Request &) { return Response{QJsonArray{row}}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method != "GET") { ++mutations; return Response{QJsonObject{}, 403}; }
            ++reads;
            if (scenario == "forbidden") return Response{QJsonArray{}};
            auto rows = scenario == "loop-marker" ? QJsonArray{} : QJsonArray{fact};
            if (scenario == "duplicate-emoji") { auto duplicate = fact; duplicate["uuid"] = topicId; rows.append(duplicate); }
            Response response{rows};
            if (scenario == "loop-marker") response.headers.insert("X-Pagination-Marker", returnedId.toUtf8());
            return response;
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QVERIFY(!modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        QCOMPARE(mutations, scenario == "forbidden" ? 1 : 0);
        QVERIFY(reads <= 2);
    }

    void reactionsDoNotMutateAfterRouteChangeOrLeakAcrossGeneration()
    {
        Fixture fixture; fixture.extraTopic = true;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int mutations = 0, preflights = 0;
        fixture.messages = [&](const Request &request) {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            auto value = row; if (topic == secondTopicId) value["topic_uuid"] = secondTopicId;
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{value}} : Response{row};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{QJsonObject{}, 503, 150}; }
            ++preflights; return Response{QJsonArray{}, 200, 100};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_COMPARE(preflights, 1); fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(mutations, 0);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.toggleReaction(messageId, "heart"); QTRY_COMPARE(mutations, 1);
        fixture.api.setAccessToken("replacement-test-owner-token");
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("reactions").toList().first().toMap().value("count").toInt(), 0);
        QCOMPARE(mutations, 1);
        fixture.controller.logout(); QVERIFY(fixture.controller.messages().isEmpty());
    }

    void sameOwnerTokenRotationRetainsUncertainReactionWithoutAnotherPost()
    {
        Fixture fixture; bool rotate = false, applied = false; int mutations = 0;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.authentication = [&](const Request &request) {
            if (rotate && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-rotated-project-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{fact, 201, 250}; }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart"); QTRY_COMPARE(mutations, 1);
        QSignalSpy authenticated(&fixture.session, &Session::authenticated);
        rotate = true; fixture.session.requestRefresh(); QTRY_COMPARE(authenticated.size(), 1);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        applied = true; row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QCOMPARE(mutations, 1);
    }

    void realtimeReactionIdentityRefreshCannotOverwriteNewerSnapshotOrContent()
    {
        Fixture fixture;
        auto row = message("kept content"); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int exactGets = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            ++exactGets;
            auto response = row; response["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "never merge GET content"}};
            return Response{response, 200, exactGets == 1 ? 100 : 0};
        };
        QVERIFY(fixture.start());
        auto payload = QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"source_name", "native"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.created", payload)));
        QTRY_COMPARE(exactGets, 1);
        row["reactions"] = QJsonObject{{"heart", 2}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId, returnedId}}};
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "newer concurrent content"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", row, 13)));
        QTRY_VERIFY(exactGets >= 2);
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        const auto model = modelMessage(fixture.controller, messageId);
        QCOMPARE(model.value("content").toString(), "newer concurrent content");
        QCOMPARE(model.value("reactions").toList().first().toMap().value("count").toInt(), 2);
        payload["source_name"] = "arbitrary-image-url";
        QVERIFY(!fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.deleted", payload, 14)));
        payload["source_name"] = "zulip"; payload["old_message_uuid"] = "not-a-uuid";
        QVERIFY(!fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.updated", payload, 15)));
    }

    void reactionsUseOwnFactUuidAndPreserveEncryptedContent()
    {
        Fixture fixture; fixture.encrypted = true;
        auto row = message(); row["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId},
                {"content", QString::fromLatin1(QByteArray("must-not-be-shown").toBase64())}};
        row["reactions"] = QJsonObject{{"future-provider-emoji", 2}};
        row["reaction_users"] = QJsonObject{}; // Count-only snapshot cannot establish ownership.
        const QJsonObject own{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "future-provider-emoji"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        bool deleted = false, validRequests = true; int mutations = 0;
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "DELETE") {
                validRequests = validRequests && request.target.path().section('/', -1) == returnedId;
                deleted = true; ++mutations; row["reactions"] = QJsonObject{{"future-provider-emoji", 1}};
                return Response{QJsonObject{}, 204};
            }
            const QUrlQuery query(request.target);
            validRequests = validRequests && query.queryItemValue("message_uuid") == messageId && query.queryItemValue("user_uuid") == userId;
            return Response{deleted ? QJsonArray{} : QJsonArray{own}};
        };
        QVERIFY(fixture.start());
        auto model = modelMessage(fixture.controller, messageId);
        QVERIFY(model.value("canReact").toBool()); QVERIFY(!model.value("reactions").toList().first().toMap().value("ownKnown").toBool());
        fixture.controller.toggleReaction(messageId, "future-provider-emoji");
        QTRY_COMPARE(mutations, 1);
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        model = modelMessage(fixture.controller, messageId);
        QVERIFY(!model.value("content").toString().contains("must-not-be-shown"));
        QVERIFY(!model.value("content").toString().contains(QString::fromLatin1(QByteArray("must-not-be-shown").toBase64())));
        QCOMPARE(model.value("reactions").toList().first().toMap().value("count").toInt(), 1);
        QVERIFY(!fixture.controller.canSend());
    }

    void reactionsCanonicalAddWaitsForProjectionAndNeverOverwritesMessage()
    {
        Fixture fixture;
        auto row = message("initial"); row["read"] = false;
        row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        bool applied = false, validBody = true; int mutations = 0, confirmations = 0;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "thumbs_up"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            ++confirmations;
            auto stale = row; stale["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "stale"}}; stale["read"] = false;
            if (confirmations >= 2) { applied = true; stale["reactions"] = QJsonObject{{"thumbs_up", 1}}; stale["reaction_users"] = QJsonObject{{"thumbs_up", QJsonArray{userId}}}; }
            return Response{stale};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                validBody = request.body == QJsonObject{{"message_uuid", messageId}, {"emoji_name", "thumbs_up"}}; ++mutations;
                return Response{fact, 201};
            }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.toggleReaction(messageId, " :Thumbs-Up: ");
        fixture.controller.toggleReaction(messageId, "thumbs_up");
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "concurrent edit"}}; row["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", row)));
        QTRY_COMPARE(mutations, 1);
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        const auto model = modelMessage(fixture.controller, messageId);
        QCOMPARE(model.value("content").toString(), "concurrent edit"); QVERIFY(model.value("read").toBool());
        const auto reaction = model.value("reactions").toList().first().toMap();
        QCOMPARE(reaction.value("emojiName").toString(), "thumbs_up"); QVERIFY(reaction.value("own").toBool());
        QVERIFY(confirmations >= 2); QCOMPARE(mutations, 1); QVERIFY(validBody);
    }

    void ambiguousReactionKeepsIntentAndExplicitRetryOnlyConfirms()
    {
        Fixture fixture;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int mutations = 0, gets = 0; bool applied = false;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{QJsonObject{}, 503}; }
            ++gets; return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        QCOMPARE(mutations, 1); QVERIFY(gets >= 2 && gets <= 5);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        applied = true; row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QCOMPARE(mutations, 1);
    }
};

QTEST_MAIN(ControllerReactionsTest)
#include "native-controller-reactions-test.moc"
