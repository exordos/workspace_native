#include "native-drafts.h"
#include "mock-server.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>

using namespace workspace;
using namespace workspace::test;

namespace {
QJsonObject draft(const QString &text, const QString &id = returnedId, int revision = 1)
{
    return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"stream_uuid", streamId},
        {"topic_uuid", topicId}, {"revision", revision}, {"created_at", "2026-01-02T10:00:00.000Z"},
        {"updated_at", "2026-01-02T10:00:01.123456Z"},
        {"payload", QJsonObject{{"kind", "markdown"}, {"content", text}}}};
}
Response snapshot(const QJsonObject &row, int status = 200)
{
    Response result{status == 412 ? QJsonObject{{"current", row}} : row, status}; result.headers.insert("ETag", '"' + QByteArray::number(row.value("revision").toInt()) + '"');
    return result;
}
QString ownerFor(const ApiClient &api, const QString &user = userId)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{
        api.server().toString(QUrl::FullyEncoded), projectId, user}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
struct Backend {
    MockServer server;
    QHash<QString, QJsonObject> rows;
    std::function<std::optional<Response>(const Request &)> override;
    bool owned = true;
    Backend()
    {
        server.handler = [this](const Request &request) -> Response {
            owned &= !QUrlQuery(request.target).hasQueryItem("project_id");
            if (!owned) return {QJsonObject{}, 403};
            if (override) if (auto response = override(request)) return *response;
            const auto path = request.target.path();
            if (!path.startsWith("/api/workspace/v1/messenger/drafts/")) return {QJsonObject{}, 404};
            const auto id = path.section('/', -1);
            if (request.method == "GET" && id.isEmpty()) {
                QJsonArray result; auto ids = rows.keys(); std::sort(ids.begin(), ids.end());
                for (const auto &key : ids) result.append(rows.value(key));
                return {result};
            }
            if (request.method == "POST" && id.isEmpty()) {
                const auto key = request.body.value("uuid").toString();
                if (rows.contains(key)) return snapshot(rows[key], rows[key].value("payload") == request.body.value("payload") ? 200 : 412);
                auto row = draft(request.body.value("payload").toObject().value("content").toString(), key);
                row["stream_uuid"] = request.body.value("stream_uuid"); row["topic_uuid"] = request.body.value("topic_uuid");
                rows[key] = row; return snapshot(row, 201);
            }
            if (!rows.contains(id)) return {QJsonObject{}, 404};
            if (request.method == "GET") return snapshot(rows[id]);
            const auto etag = '"' + QByteArray::number(rows[id].value("revision").toInt()) + '"';
            if (request.headers.value("if-match") != etag) return snapshot(rows[id], 412);
            if (request.method == "PUT") {
                rows[id]["payload"] = request.body.value("payload"); rows[id]["revision"] = rows[id].value("revision").toInt() + 1;
                return snapshot(rows[id]);
            }
            if (request.method == "DELETE") { rows.remove(id); return {QJsonObject{}, 204}; }
            return {QJsonObject{}, 405};
        };
    }
    int count(const QByteArray &method, bool collection = false) const
    {
        return std::count_if(server.requests.cbegin(), server.requests.cend(), [&](const Request &r) {
            return r.method == method && (!collection || r.target.path().endsWith("/drafts/"));
        });
    }
};
struct Harness {
    ApiClient api{nullptr, true};
    NativeDraftService storage;
    NativeDrafts model{&storage};
    Harness(const QString &directory, Backend &backend) : storage(directory)
    {
        api.setServer(backend.server.origin()); api.setAccessToken("synthetic-draft-token");
        model.setOwner(ownerFor(api)); model.select(streamId, topicId);
        model.useRemote(&api, projectId, userId, {streamId});
    }
    ~Harness()
    {
        model.pauseRemote(true);
        if (!storage.stopped()) {
            bool finished = false; model.flush(&model, [&](bool) { finished = true; });
            const bool flushed = QTest::qWaitFor([&] { return finished; }, 5000); Q_UNUSED(flushed);
            storage.shutdown(); const bool stopped = QTest::qWaitFor([&] { return storage.stopped(); }, 5000); Q_UNUSED(stopped);
        }
    }
    bool boot()
    {
        return QTest::qWaitFor([&] { return model.ready(streamId, topicId) && !model.remoteStatus().busy; }, 5000)
            && model.remoteStatus().failure == NativeDrafts::RemoteFailure::None;
    }
    bool flush()
    {
        bool done = false, okay = false; model.flush(&model, [&](bool success) { done = true; okay = success; });
        return QTest::qWaitFor([&] { return done; }, 5000) && okay;
    }
    std::optional<DraftRecord> read(const QString &id)
    {
        bool done = false; NativeDraftStore::Read value;
        storage.read(storage.lease(), id, &model, [&](NativeDraftStore::Read r) { value = std::move(r); done = true; });
        if (!QTest::qWaitFor([&] { return done; }, 5000) || value.failure != NativeDraftStore::Failure::None) return {};
        return value.record;
    }
};
QJsonObject diskRecord(const QString &directory, const QString &owner, const QString &id)
{
    const auto name = QUuid::createUuid().toString(); QJsonObject result;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(directory + '/' + owner + ".sqlite3");
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (db.open()) {
            QSqlQuery query(db); query.prepare("SELECT payload FROM draft_records WHERE uuid=?"); query.addBindValue(id);
            if (query.exec() && query.next()) result = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name); return result;
}
}

class NativeDraftSyncTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }

    void bootstrapAndNavigationUseOwnedLocalDrafts()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Server draft");
        Harness f(directory.path(), backend); QVERIFY(f.boot()); QCOMPARE(f.model.text(), "Server draft");
        QVERIFY(backend.owned); QCOMPARE(backend.count("GET", true), 1);
        QCOMPARE(QUrlQuery(backend.server.requests.first().target).queryItemValue("page_limit"), "100");
        f.model.select(streamId, secondTopicId); QTRY_VERIFY(f.model.ready(streamId, secondTopicId));
        f.model.select(streamId, topicId); QTRY_VERIFY(f.model.ready(streamId, topicId));
        QCOMPARE(f.model.text(), "Server draft"); QTest::qWait(50); QCOMPARE(backend.count("GET", true), 1);
    }

    void ordinaryInputDebouncesThenKeepsExactLocalSpacing()
    {
        QTemporaryDir directory; Backend backend; Harness f(directory.path(), backend); QVERIFY(f.boot());
        QVERIFY(f.model.edit(streamId, topicId, "  Unsent 😀\n"));
        QTest::qWait(450); QCOMPARE(backend.count("POST"), 0);
        QTRY_COMPARE_WITH_TIMEOUT(backend.count("POST"), 1, 4000);
        QTRY_COMPARE(f.model.remoteStatus().phase, DraftRecord::Phase::Saved);
        QCOMPARE(f.model.text(), "  Unsent 😀\n"); QVERIFY(!backend.rows.isEmpty());
        QCOMPARE(backend.rows.cbegin().value().value("payload").toObject().value("content"), "Unsent 😀");
        const auto token = f.model.capture(streamId, topicId); QVERIFY(f.flush()); const auto row = f.read(token.uuid); QVERIFY(row);
        QCOMPARE(row->text, f.model.text()); QCOMPARE(row->serverUpdatedAt, "2026-01-02T10:00:01.123456Z");
        QCOMPARE(row->revision, 1); QVERIFY(!row->createPending);
    }

    void lateBootstrapCannotReplaceNewerInput()
    {
        QTemporaryDir directory; Backend backend; std::function<void()> release;
        const auto finish = qScopeGuard([&] { if (release) release(); });
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "GET" || !r.target.path().endsWith("/drafts/")) return {};
            Response response{QJsonArray{draft("Older server draft")}};
            response.defer = [&](auto send) { release = std::move(send); }; return response;
        };
        Harness f(directory.path(), backend); QTRY_VERIFY(bool(release));
        QVERIFY(f.model.edit(streamId, topicId, "New local input")); release(); release = {};
        QVERIFY(f.boot()); QCOMPARE(f.model.text(), "New local input");
        QTRY_COMPARE_WITH_TIMEOUT(f.model.remoteStatus().phase, DraftRecord::Phase::Saved, 5000);
        QCOMPARE(f.model.text(), "New local input");
    }

    void uncertainCreateUsesItsFrozenPayloadAcrossTwoStorageLifetimes()
    {
        QTemporaryDir directory; Backend backend; std::function<void()> release; QString id;
        const auto finish = qScopeGuard([&] { if (release) release(); });
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "POST" || backend.count("POST") != 1) return {};
            id = r.body.value("uuid").toString(); backend.rows[id] = draft("First payload", id);
            auto response = snapshot(backend.rows[id], 201); response.defer = [&](auto send) { release = std::move(send); }; return response;
        };
        {
            Harness f(directory.path(), backend); QVERIFY(f.boot()); QVERIFY(f.model.edit(streamId, topicId, "First payload"));
            QTRY_VERIFY_WITH_TIMEOUT(bool(release), 4000); QVERIFY(f.model.edit(streamId, topicId, "Newer input")); QVERIFY(f.flush());
        }
        Harness f(directory.path(), backend); QVERIFY(f.boot()); QCOMPARE(f.model.text(), "Newer input");
        QTRY_COMPARE_WITH_TIMEOUT(backend.count("PUT"), 1, 5000);
        QTRY_COMPARE(f.model.remoteStatus().phase, DraftRecord::Phase::Saved);
        QCOMPARE(backend.count("POST"), 2);
        QVector<Request> creates; for (const auto &r : backend.server.requests) if (r.method == "POST") creates.append(r);
        QCOMPARE(creates[0].body, creates[1].body); QCOMPARE(creates[0].body.value("uuid"), id);
        QCOMPARE(backend.rows[id].value("payload").toObject().value("content"), "Newer input");
        for (const auto &r : backend.server.requests) if (r.method == "PUT") QCOMPARE(r.headers.value("if-match"), "\"1\"");
    }

    void conditionalConflictRequiresAnExplicitChoice_data()
    {
        QTest::addColumn<bool>("server"); QTest::newRow("server") << true; QTest::newRow("local") << false;
    }
    void conditionalConflictRequiresAnExplicitChoice()
    {
        QFETCH(bool, server); QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Initial");
        Harness f(directory.path(), backend); QVERIFY(f.boot());
        backend.rows[returnedId] = draft("Other client", returnedId, 2);
        QVERIFY(f.model.edit(streamId, topicId, "Local edit"));
        QTRY_VERIFY_WITH_TIMEOUT(f.model.remoteStatus().conflict, 5000);
        QCOMPARE(f.model.text(), "Local edit"); QCOMPARE(f.model.remoteStatus().serverText, "Other client");
        QVERIFY(f.flush()); const auto row = f.read(returnedId); QVERIFY(row);
        QCOMPARE(row->conflictRevision, 2); QCOMPARE(row->conflictText, "Other client");
        QTest::qWait(200); QCOMPARE(backend.count("PUT"), 1);
        QVERIFY(f.model.resolveConflict(server));
        QTRY_COMPARE_WITH_TIMEOUT(f.model.remoteStatus().phase, DraftRecord::Phase::Saved, 5000);
        QCOMPARE(f.model.text(), server ? "Other client" : "Local edit");
        QCOMPARE(backend.count("PUT"), server ? 1 : 2);
        QCOMPARE(backend.rows[returnedId].value("payload").toObject().value("content"), server ? "Other client" : "Local edit");
    }

    void consumptionIsDurableBeforeDeleteAndNewInputSurvives()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Sent text");
        Harness f(directory.path(), backend); QVERIFY(f.boot()); bool durable = false;
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method == "DELETE") {
                const auto onDisk = diskRecord(directory.path(), f.model.owner(), returnedId);
                durable = onDisk.value("consumed").toBool() && onDisk.value("phase").toInt() == int(DraftRecord::Phase::Deleting);
            }
            return {};
        };
        QVERIFY(f.model.consume(f.model.capture(streamId, topicId)));
        QVERIFY(f.model.edit(streamId, topicId, "New input after send"));
        QTRY_COMPARE_WITH_TIMEOUT(backend.count("DELETE"), 1, 5000); QVERIFY(durable);
        QTRY_COMPARE_WITH_TIMEOUT(f.model.remoteStatus().phase, DraftRecord::Phase::Saved, 5000);
        QCOMPARE(f.model.text(), "New input after send"); QVERIFY(!backend.rows.contains(returnedId));
    }

    void privatePromotionDoesNotReplayAnUncertainPlaintextCreate()
    {
        QTemporaryDir directory; Backend backend; std::function<void()> release; QString id;
        const auto finish = qScopeGuard([&] { if (release) release(); });
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "POST") return {};
            id = r.body.value("uuid").toString(); backend.rows[id] = draft("Earlier public body", id);
            auto response = snapshot(backend.rows[id], 201); response.defer = [&](auto send) { release = std::move(send); }; return response;
        };
        {
            Harness f(directory.path(), backend); QVERIFY(f.boot()); QVERIFY(f.model.edit(streamId, topicId, "Earlier public body"));
            QTRY_VERIFY_WITH_TIMEOUT(bool(release), 5000); QVERIFY(f.model.edit(streamId, topicId, ":::private synthetic input:::")); QVERIFY(f.flush());
            const auto row = f.read(id); QVERIFY(row); QVERIFY(row->privateDeletion); QVERIFY(row->privateCreateUncertain);
            QVERIFY(row->text.isEmpty()); QVERIFY(row->createText.isEmpty()); QVERIFY(row->replySession.isEmpty());
        }
        Harness f(directory.path(), backend); QVERIFY(f.boot());
        QTRY_COMPARE_WITH_TIMEOUT(backend.count("DELETE"), 1, 5000);
        QCOMPARE(backend.count("POST"), 1); QVERIFY(!backend.rows.contains(id)); QVERIFY(f.model.text().isEmpty());
        QVERIFY(backend.count("GET") >= 3); // two bootstraps and one owned exact lookup
    }

    void canonicalizedEchoDoesNotCauseAnEndlessUpdateLoop()
    {
        QTemporaryDir directory; Backend backend;
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "POST") return {};
            const auto id = r.body.value("uuid").toString(); backend.rows[id] = draft("Canonical text", id); return snapshot(backend.rows[id], 201);
        };
        Harness f(directory.path(), backend); QVERIFY(f.boot()); QVERIFY(f.model.edit(streamId, topicId, "Original text"));
        QTRY_COMPARE_WITH_TIMEOUT(f.model.remoteStatus().phase, DraftRecord::Phase::Saved, 5000);
        QTest::qWait(150); QCOMPARE(backend.count("POST"), 1); QCOMPARE(backend.count("PUT"), 0);
        QCOMPARE(f.model.text(), "Original text");
    }

    void accountReplacementFencesAnOldHeldResponse()
    {
        QTemporaryDir directory; Backend backend; std::function<void()> release;
        const auto finish = qScopeGuard([&] { if (release) release(); });
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "POST") return {};
            auto response = snapshot(draft("Old owner", r.body.value("uuid").toString()), 201);
            response.defer = [&](auto send) { release = std::move(send); }; return response;
        };
        Harness f(directory.path(), backend); QVERIFY(f.boot()); QVERIFY(f.model.edit(streamId, topicId, "Old owner"));
        QTRY_VERIFY_WITH_TIMEOUT(bool(release), 5000);
        f.api.setAccessToken("synthetic-other-account-token"); f.model.setOwner(ownerFor(f.api, secondTopicId));
        f.model.select(streamId, topicId); f.model.useRemote(nullptr, {}, {}, {});
        QVERIFY(f.model.edit(streamId, topicId, "New account input")); release(); release = {};
        QVERIFY(f.flush()); QCOMPARE(f.model.text(), "New account input"); QCOMPARE(f.model.remoteStatus().phase, DraftRecord::Phase::Local);
    }

    void wrongOwnerCannotEnableRemoteRequests()
    {
        QTemporaryDir directory; Backend backend; ApiClient api(nullptr, true); QVERIFY(api.setServer(backend.server.origin()));
        api.setAccessToken("synthetic-token"); NativeDraftService storage(directory.path()); NativeDrafts model(&storage);
        model.setOwner(QString(64, 'a')); model.select(streamId, topicId); model.useRemote(&api, projectId, userId, {streamId});
        QVERIFY(model.edit(streamId, topicId, "Only local")); QTest::qWait(1100);
        QCOMPARE(backend.server.requests.size(), 0); QVERIFY(!model.remoteStatus().enabled);
    }

    void anotherValidEditRetriesAnEarlierRejectedUpdate()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Initial");
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method == "PUT" && backend.count("PUT") == 1) return Response{QJsonObject{}, 400};
            return {};
        };
        Harness f(directory.path(), backend); QVERIFY(f.boot()); QVERIFY(f.model.edit(streamId, topicId, "Rejected edit"));
        QTRY_COMPARE_WITH_TIMEOUT(f.model.remoteStatus().phase, DraftRecord::Phase::Failed, 5000);
        QTest::qWait(150); QCOMPARE(backend.count("PUT"), 1);
        QVERIFY(f.model.edit(streamId, topicId, "A newer valid edit"));
        QTRY_COMPARE_WITH_TIMEOUT(f.model.remoteStatus().phase, DraftRecord::Phase::Saved, 5000);
        QCOMPARE(backend.count("PUT"), 2); QCOMPARE(f.model.text(), "A newer valid edit");
    }

    void returningToAnOwnerResumesItsCancelledCreate()
    {
        QTemporaryDir directory; Backend backend; std::function<void()> release;
        const auto finish = qScopeGuard([&] { if (release) release(); });
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "POST" || backend.count("POST") != 1) return {};
            const auto id = r.body.value("uuid").toString(); backend.rows[id] = draft("Owned input", id);
            auto response = snapshot(backend.rows[id], 201); response.defer = [&](auto send) { release = std::move(send); }; return response;
        };
        Harness f(directory.path(), backend); QVERIFY(f.boot()); QVERIFY(f.model.edit(streamId, topicId, "Owned input"));
        QTRY_VERIFY_WITH_TIMEOUT(bool(release), 5000); QVERIFY(f.flush()); const auto originalOwner = f.model.owner();
        f.model.setOwner(QString(64, 'b')); f.model.select(streamId, topicId);
        f.model.setOwner(originalOwner); f.model.select(streamId, topicId);
        f.model.useRemote(&f.api, projectId, userId, {streamId});
        QTRY_COMPARE_WITH_TIMEOUT(f.model.remoteStatus().phase, DraftRecord::Phase::Saved, 5000);
        QCOMPARE(backend.count("POST"), 2); QCOMPARE(f.model.text(), "Owned input");
    }

    void aFailedCollectionCannotPrunePreviouslySavedInput()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Saved input");
        Harness f(directory.path(), backend); QVERIFY(f.boot());
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "GET") return {};
            if (QUrlQuery(r.target).queryItemValue("page_marker") == "next") return Response{QJsonObject{}, 503};
            Response page{QJsonArray{}}; page.headers.insert("X-Pagination-Marker", "next"); return page;
        };
        f.model.refetchRemote(); QTRY_COMPARE(f.model.remoteStatus().failure, NativeDrafts::RemoteFailure::Unavailable);
        QCOMPARE(f.model.text(), "Saved input"); QVERIFY(f.flush()); QVERIFY(f.read(returnedId));
    }

    void explicitRefetchRemovesAnAbsentSavedDraft()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Saved input");
        Harness f(directory.path(), backend); QVERIFY(f.boot()); backend.rows.clear(); f.model.refetchRemote();
        QVERIFY(f.boot()); QTRY_VERIFY(f.model.text().isEmpty()); QVERIFY(f.flush()); QVERIFY(!f.read(returnedId));
    }

    void deleteConflictRetainsBothVersionsUntilChosen()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Sent input");
        Harness f(directory.path(), backend); QVERIFY(f.boot()); backend.rows[returnedId] = draft("Changed on another client", returnedId, 2);
        QVERIFY(f.model.consume(f.model.capture(streamId, topicId)));
        QTRY_VERIFY(f.model.remoteStatus().conflict); QCOMPARE(backend.count("DELETE"), 1);
        QCOMPARE(f.model.remoteStatus().serverText, "Changed on another client"); QVERIFY(f.model.text().isEmpty());
        QVERIFY(f.model.resolveConflict(true)); QTRY_COMPARE(f.model.text(), "Changed on another client");
        QTest::qWait(100); QCOMPARE(backend.count("DELETE"), 1);
    }

    void remoteSuccessCannotConsumeANewerSameTextSnapshot()
    {
        QTemporaryDir directory; Backend backend; std::function<void()> release;
        const auto finish = qScopeGuard([&] { if (release) release(); });
        backend.override = [&](const Request &r) -> std::optional<Response> {
            if (r.method != "POST") return {};
            const auto id = r.body.value("uuid").toString(); backend.rows[id] = draft("A", id);
            auto response = snapshot(backend.rows[id], 201); response.defer = [&](auto send) { release = std::move(send); }; return response;
        };
        Harness f(directory.path(), backend); QVERIFY(f.boot()); QVERIFY(f.model.edit(streamId, topicId, "A"));
        const auto sent = f.model.capture(streamId, topicId); QTRY_VERIFY_WITH_TIMEOUT(bool(release), 5000);
        QVERIFY(f.model.edit(streamId, topicId, "B")); QVERIFY(f.model.edit(streamId, topicId, "A"));
        release(); release = {}; QTRY_COMPARE(f.model.remoteStatus().phase, DraftRecord::Phase::Saved);
        QVERIFY(!f.model.consume(sent)); QCOMPARE(f.model.text(), "A");
    }

    void rejectedDeletionRemainsVisibleAndCanBeRetried()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Already sent");
        Harness f(directory.path(), backend); QVERIFY(f.boot());
        backend.override = [](const Request &r) -> std::optional<Response> {
            if (r.method == "DELETE") return Response{QJsonObject{}, 400};
            return {};
        };
        QVERIFY(f.model.consume(f.model.capture(streamId, topicId)));
        QTRY_COMPARE(f.model.remoteStatus().failure, NativeDrafts::RemoteFailure::Rejected);
        QVERIFY(f.flush()); const auto failed = f.read(returnedId); QVERIFY(failed); QVERIFY(failed->consumed);
        QCOMPARE(failed->phase, DraftRecord::Phase::Failed);
        f.model.showPage(); QTRY_COMPARE(f.model.page().value("rows").toList().size(), 1);
        const auto preview = f.model.page().value("rows").toList().first().toMap();
        QCOMPARE(preview.value("phase").toInt(), int(DraftRecord::Phase::Failed)); QVERIFY(preview.value("consumed").toBool());
        QTest::qWait(100); QCOMPARE(backend.count("DELETE"), 1);
        backend.override = {}; QVERIFY(f.model.actOnDraft(returnedId, NativeDrafts::Action::Delete));
        QTRY_VERIFY(!backend.rows.contains(returnedId)); QTRY_COMPARE(f.model.page().value("rows").toList().size(), 0);
        QCOMPARE(backend.count("DELETE"), 2); QVERIFY(f.flush()); QVERIFY(!f.read(returnedId));
    }

    void encryptionChangePreservesPriorOrdinaryWorkWithoutSyncingIt()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Prior ordinary work");
        Harness f(directory.path(), backend); QVERIFY(f.boot());
        f.model.useRemote(&f.api, projectId, userId, {}, QSet<QString>{streamId});
        QVERIFY(f.boot()); QVERIFY(f.flush()); const auto prior = f.read(returnedId); QVERIFY(prior);
        QCOMPARE(prior->text, "Prior ordinary work"); QVERIFY(!f.model.edit(streamId, topicId, "Encrypted input must stay in controller memory"));
        QCOMPARE(backend.count("POST"), 0); QCOMPARE(backend.count("PUT"), 0); QCOMPARE(backend.count("DELETE"), 0);
        f.model.useRemote(&f.api, projectId, userId, {streamId}, QSet<QString>{streamId}); QVERIFY(f.boot());
        QCOMPARE(f.model.text(), "Prior ordinary work");
    }

    void authoritativeAccessLossClearsLocalStateEvenWhenDraftApiFails()
    {
        QTemporaryDir directory; Backend backend; backend.rows[returnedId] = draft("Initially accessible");
        Harness f(directory.path(), backend); QVERIFY(f.boot());
        QVERIFY(f.model.edit(streamId, topicId, ":::private synthetic editor:::")); QVERIFY(f.flush());
        backend.override = [](const Request &) -> std::optional<Response> { return Response{QJsonObject{}, 503}; };
        f.model.useRemote(&f.api, projectId, userId, {});
        QTRY_VERIFY(f.model.text().isEmpty()); QVERIFY(f.flush()); QVERIFY(!f.read(returnedId));
        QVERIFY(!f.model.edit(streamId, topicId, "Cannot write an inaccessible route"));
        QCOMPARE(backend.count("POST"), 0); QCOMPARE(backend.count("PUT"), 0);
    }
};
QTEST_GUILESS_MAIN(NativeDraftSyncTest)
#include "native-draft-sync-test.moc"
