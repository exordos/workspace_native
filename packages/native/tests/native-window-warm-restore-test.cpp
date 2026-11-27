#include "gui-phase-trace.h"
#include "native-app.h"
#include "native-realtime-cursor-store.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-realtime-fixtures.h"
#include "workspace-sessions.h"

#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>

namespace {

QQuickItem *visualItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = visualItem(child, name)) return found;
    return nullptr;
}

QString fixtureId(int family, int number)
{
    return QStringLiteral("a000000%1-0000-4000-8000-%2")
        .arg(family).arg(number + 1, 12, 16, QChar('0'));
}

}

class WarmRestoreWindowTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void warmSavedStreamOpensWithoutBlockingTheWindow()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Fixture fixture;
        fixture.controller.setTopicPreloadEnabledForTest(true);
        RealtimeGateway gateway(fixture.server.serverPort());
        LoopbackKeyTls tls(gateway.serverPort());
        QVERIFY(tls.isListening());

        QJsonArray streams, topics, history, users;
        for (int i = 0; i < 120; ++i) {
            streams.append(QJsonObject{{"uuid", i ? fixtureId(1, i) : streamId}, {"project_id", projectId},
                {"name", QStringLiteral("Channel %1").arg(i)}, {"role", "owner"}, {"encryption", false}});
        }
        for (int i = 0; i < 364; ++i) {
            topics.append(QJsonObject{{"uuid", i ? fixtureId(2, i) : topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"user_uuid", userId}, {"name", QStringLiteral("Topic %1").arg(i)},
                {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"is_default", false}, {"is_done", false}});
        }
        for (int i = 0; i < 100; ++i) {
            auto row = message(QStringLiteral("Saved message %1 with **formatted text**.").arg(i), fixtureId(3, i));
            row["created_at"] = QDateTime(QDate(2026, 1, 2), QTime(10, 0), QTimeZone::UTC)
                .addSecs(i).toString(Qt::ISODateWithMs);
            history.append(row);
        }
        for (int i = 0; i < 800; ++i) {
            users.append(QJsonObject{{"uuid", i ? fixtureId(4, i) : userId}, {"project_id", projectId},
                {"username", QStringLiteral("member%1").arg(i)}, {"first_name", QStringLiteral("Member %1").arg(i)}});
        }

        fixture.streams = [&](const Request &) { return Response{streams}; };
        fixture.topics = [&](const Request &r) {
            return Response{QUrlQuery(r.target).queryItemValue("stream_uuid") == streamId ? topics : QJsonArray{}};
        };
        fixture.users = [&](const Request &) { return Response{users}; };
        fixture.aggregateMessages = true;
        fixture.messages = [&](const Request &r) {
            const QUrlQuery query(r.target);
            const auto requestedTopic = query.queryItemValue("topic_uuid");
            return Response{query.queryItemValue("stream_uuid") == streamId
                && (requestedTopic.isEmpty() || requestedTopic == topicId)
                && !query.hasQueryItem("mentioned") ? history : QJsonArray{}};
        };
        fixture.realtime = [](const Request &r) {
            if (r.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"}, {"epoch_version", 10},
                    {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        std::function<void()> deliverRefresh;
        fixture.authentication = [&](const Request &r) {
            auto response = sessionResponse(r);
            if (r.body.value("grant_type") == QLatin1String("refresh_token"))
                response.defer = [&](std::function<void()> deliver) { deliverRefresh = std::move(deliver); };
            return response;
        };
        {
            NativeCache seed(directory.path(), {}, true);
            QVERIFY(seed.open(tls.origin(), projectId, userId));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::Streams, {}, {}, {}}, streams));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::Topics, streamId, {}, {}}, topics));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::Users, {}, {}, {}}, users));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::History, streamId, {}, {}}, history));
        }

        ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const SavedAccount saved{SessionVault::accountId(tls.origin(), projectId, userId),
            tls.origin(), projectId, userId, "synthetic", "Warm restore"};
        vault.save(saved, "synthetic-refresh");
        QTRY_COMPARE(vault.accounts().size(), 1);
        QVERIFY(vault.setCurrentAccountId(saved.id).isEmpty());
        fixture.session.setVault(&vault);
        NativeCacheService cache(directory.path(), {}, true);
        fixture.controller.useCache(&cache);
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        const auto identity = QJsonDocument(QJsonArray{tls.origin().toString(QUrl::FullyEncoded), projectId, userId});
        const auto owner = QCryptographicHash::hash(identity.toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        preferences.recordLastConversation(QString::fromLatin1(owner.toHex()), streamId, {});
        RealtimeCursorStore cursors(directory.filePath("cursors"));
        WorkspaceSessions workspaces(fixture.session, fixture.controller, vault, cursors);
        QQmlApplicationEngine engine;
        QElapsedTimer startup;
        startup.start();
        loadNativeUi(engine, &fixture.controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        window->resize(1440, 900);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const qint64 firstWindowMs = startup.elapsed();
        QTest::mouseMove(window, QPoint(2, 2));
        fixture.controller.resetGuiPhaseTraceForTest();
        GuiStallSampler sampler(directory.filePath("stalls.json"));
        workspaces.start();
        auto *content = window->contentItem();
        const auto cachedChannel = [&] { return visualItem(content, QStringLiteral("stream-") + streamId); };
        QTRY_VERIFY(deliverRefresh && cachedChannel() && cachedChannel()->isVisible());
        QVERIFY(fixture.controller.startupCacheMode());
        QVERIFY(!fixture.controller.authenticated());
        deliverRefresh();
        QTRY_VERIFY(fixture.controller.authenticated());
        QTRY_COMPARE(fixture.controller.selectedStream(), streamId);
        QTRY_COMPARE(fixture.controller.messages().size(), history.size());
        const auto lastId = fixtureId(3, 99);
        const auto lastBubble = [&] { return visualItem(content, QStringLiteral("message-") + lastId); };
        auto *list = visualItem(content, QStringLiteral("messageList"));
        QVERIFY(list);
        QTRY_VERIFY(lastBubble() && lastBubble()->isVisible()
            && list->boundingRect().contains(lastBubble()->mapToItem(list, lastBubble()->boundingRect().center())));
        const qint64 firstHistoryMs = startup.elapsed();
        // Include asynchronous revalidation, viewport positioning and deferred delegates.
        QTest::qWait(1500);
        const auto gaps = sampler.summary();
        const auto phases = fixture.controller.guiPhaseTraceForTest().value("spans").toMap();
        const QJsonObject report{{"first_window_ms", firstWindowMs}, {"first_history_ms", firstHistoryMs},
            {"stall", QJsonObject::fromVariantMap(gaps)}, {"phases", QJsonObject::fromVariantMap(phases)}};
        qInfo().noquote() << "WARM_RESTORE" << QJsonDocument(report).toJson(QJsonDocument::Compact);
        QTRY_VERIFY(list->property("cacheBuffer").toInt() >= qCeil(list->height()));
        QCOMPARE(fixture.controller.selectedTopic(), QString{});
        QCOMPARE(fixture.controller.topics().size(), topics.size());
        QVERIFY(phases.contains("startup.catalog-publish-cached"));
        QVERIFY(phases.contains("route.saved-route-restore"));
        QVERIFY(gaps.value("samples").toInt() > 0);
        // Contended software-rendered CI uses a wider guard; the profiled desktop enforces 300 ms.
        const int budget = qEnvironmentVariableIntValue("WORKSPACE_NATIVE_STRICT_STALL_BUDGET") == 1 ? 300 : 3000;
        QVERIFY2(gaps.value("worst_gap_ms").toDouble() < budget, qPrintable(QString::fromUtf8(
            QJsonDocument(report).toJson(QJsonDocument::Compact))));
        const auto capsule = QStringLiteral("quickReactionCapsule-") + lastId;
        QVERIFY(!visualItem(content, capsule));
        QVERIFY(!visualItem(content, QStringLiteral("check-reaction-") + lastId));
        auto *bubble = lastBubble();
        QVERIFY(bubble);
        QTest::mouseMove(window, bubble->mapToScene(bubble->boundingRect().center()).toPoint());
        QTRY_VERIFY(visualItem(content, capsule));
        window->close();
        QCoreApplication::processEvents();
    }
};

QTEST_MAIN(WarmRestoreWindowTest)
#include "native-window-warm-restore-test.moc"
