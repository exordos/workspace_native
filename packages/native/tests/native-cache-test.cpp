#include "native-cache.h"

#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QProcess>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QSignalSpy>
#include <QTimer>
#include <algorithm>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace workspace;
namespace {
const QString Project = "fe02e55d-4548-4b3e-a175-fcae928f41b2";
const QString User = "50ab4d98-cdd1-4588-bfa3-4c885b828eff";
const QString UserB = "443bcae9-384e-48e8-b609-491788b4d6db";
const QString Stream = "8470a07e-2e5e-44c7-81d7-c35015fb63c9";
const QString Topic = "6078fd48-1541-47cd-bec6-f7967b0157a8";
const QUrl Server("https://workspace.example.test");
const NativeCache::Key Streams{NativeCache::Kind::Streams, {}, {}, {}};
const NativeCache::Key History{NativeCache::Kind::History, Stream, Topic, {}};

QJsonObject stream(const QString &name = "Desktop")
{
    return {{"uuid", Stream}, {"project_id", Project}, {"name", name}, {"encryption", false}};
}
QJsonObject message(const QString &text = "Saved message")
{
    return {{"uuid", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"project_id", Project},
        {"stream_uuid", Stream}, {"topic_uuid", Topic}, {"author_uuid", User},
        {"created_at", "2026-11-06T12:34:56.123456Z"}, {"read", false}, {"is_own", true},
        {"starred", false}, {"payload", QJsonObject{{"kind", "markdown"}, {"content", text}}}};
}

bool modify(const QString &directory, const QString &sql, bool foreignKeys = false)
{
    const auto connection = QUuid::createUuid().toString();
    bool result;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(directory + "/cache.sqlite3");
        result = database.open();
        { QSqlQuery query(database);
          result = result && (!foreignKeys || query.exec("PRAGMA foreign_keys=ON")) && query.exec(sql); }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

bool legacyWithoutRowidCache(const QString &directory)
{
    const auto connection = QUuid::createUuid().toString(); bool okay = false;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(directory + "/cache.sqlite3"); okay = database.open();
        {
            QSqlQuery query(database);
            const QStringList statements{
                "PRAGMA page_size=4096", "PRAGMA auto_vacuum=INCREMENTAL",
                "CREATE TABLE entries (owner TEXT NOT NULL,key BLOB NOT NULL,kind INTEGER NOT NULL,stream TEXT NOT NULL,topic TEXT NOT NULL,marker BLOB NOT NULL,payload BLOB NOT NULL,next_marker BLOB NOT NULL,saved_at INTEGER NOT NULL,expires_at INTEGER NOT NULL,last_used INTEGER NOT NULL,size INTEGER NOT NULL,PRIMARY KEY(owner,key)) WITHOUT ROWID",
                "CREATE INDEX cache_routes ON entries(owner,kind,stream,topic)",
                "CREATE INDEX cache_lru ON entries(last_used)",
                "CREATE TABLE history_rows (owner TEXT NOT NULL,key BLOB NOT NULL,message_uuid TEXT NOT NULL,PRIMARY KEY(owner,key,message_uuid),FOREIGN KEY(owner,key) REFERENCES entries(owner,key) ON DELETE CASCADE) WITHOUT ROWID",
                "CREATE INDEX cache_message_lookup ON history_rows(owner,message_uuid,key)",
                "PRAGMA application_id=1464746801", "PRAGMA user_version=3"};
            for (const auto &sql : statements) if (okay) { okay = query.exec(sql); query.finish(); }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection); return okay;
}
}

class NativeCacheTest final : public QObject {
    Q_OBJECT
private slots:
    void nullableSourceUserFieldsDoNotPreventAvatarReferencePersistence_data()
    {
        QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("accepted");
        for (const auto *field : {"first_name", "last_name", "email", "all-null"})
            QTest::newRow(field) << QString::fromLatin1(field) << true;
        for (const auto *field : {"first_name", "last_name", "email"})
            for (const auto *type : {"number", "object", "array"}) {
                const auto label = QString::fromLatin1(field) + "-" + QString::fromLatin1(type);
                QTest::newRow(qPrintable(label)) << label << false;
            }
        QTest::newRow("name-null-stays-invalid") << QString("name-null") << false;
        QTest::newRow("display-name-null-stays-invalid") << QString("display_name-null") << false;
    }
    void nullableSourceUserFieldsDoNotPreventAvatarReferencePersistence()
    {
        QFETCH(QString, scenario); QFETCH(bool, accepted);
        const QString avatar = "urn:image:aaaaaaaa-0000-4000-8000-000000000001";
        const NativeCache::Key users{NativeCache::Kind::Users, {}, {}, {}};
        QJsonObject person{{"uuid", User}, {"project_id", Project}, {"username", "nullable-source-user"},
            {"first_name", "Cassandra"}, {"last_name", "Volkova"}, {"email", "nullable@example.invalid"}, {"avatar", avatar}};
        if (scenario == "all-null") {
            for (const auto *field : {"first_name", "last_name", "email"}) person[field] = QJsonValue::Null;
        } else if (accepted) person[scenario] = QJsonValue::Null;
        else if (scenario.endsWith("-null")) person[scenario.left(scenario.size() - 5)] = QJsonValue::Null;
        else {
            const auto separator = scenario.lastIndexOf('-');
            const auto field = scenario.left(separator), type = scenario.mid(separator + 1);
            person[field] = type == "number" ? QJsonValue(7) : type == "object"
                ? QJsonValue(QJsonObject{{"nested", "not-a-string"}}) : QJsonValue(QJsonArray{"not-a-string"});
        }
        QTemporaryDir directory;
        {
            NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
            if (!accepted) {
                QVERIFY(!cache.put(cache.lease(), users, {person}));
                QVERIFY(!cache.get(cache.lease(), users)); QVERIFY(cache.available());
                QCOMPARE(cache.stats().entries, 0); return;
            }
            QVERIFY(cache.put(cache.lease(), users, {person})); // Distinct original nullable-DTO admission RED.
        }
        NativeCache reopened(directory.path()); QVERIFY(reopened.open(Server, Project, User));
        const auto saved = reopened.get(reopened.lease(), users); QVERIFY(saved); QCOMPARE(saved->rows.size(), 1);
        const auto actual = saved->rows.first().toObject();
        for (const auto *field : {"uuid", "project_id", "username", "first_name", "last_name", "email"}) {
            QVERIFY(actual.contains(field)); QCOMPARE(actual.value(field), person.value(field));
        }
        QCOMPARE(actual.value("avatar").toString(), avatar);
        QVERIFY(!saved->fresh); QVERIFY(reopened.diskUsageBytes() <= reopened.diskLimitBytes());
    }

    void optionalImageAvatarReferenceSurvivesOwnedUsersCacheReopen_data()
    {
        QTest::addColumn<QJsonValue>("wireAvatar"); QTest::addColumn<QString>("expected");
        const QString image = "urn:image:aaaaaaaa-0000-4000-8000-000000000001";
        QTest::newRow("image") << QJsonValue(image) << image;
        QTest::newRow("feff-trim") << QJsonValue(QString(QChar(0xfeff)) + image + QChar(0xfeff)) << image;
        QTest::newRow("nel-not-trimmed") << QJsonValue(QString(QChar(0x85)) + image + QChar(0x85)) << QString{};
        QTest::newRow("nonstring-object") << QJsonValue(QJsonObject{{"image", image}}) << QString{};
        QTest::newRow("extra-suffix") << QJsonValue(image + "/extra") << QString{};
        QTest::newRow("nil-uuid-reference") << QJsonValue("urn:image:00000000-0000-0000-0000-000000000000")
            << QString("urn:image:00000000-0000-0000-0000-000000000000");
        const QString upperUuid = "urn:image:" + image.mid(10).toUpper();
        QTest::newRow("uppercase-uuid") << QJsonValue(upperUuid) << upperUuid;
        QTest::newRow("outer-whitespace") << QJsonValue(" " + image + "\n") << image;
        QTest::newRow("absent") << QJsonValue(QJsonValue::Undefined) << QString{};
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << QString{};
        QTest::newRow("invalid-id") << QJsonValue("urn:image:not-a-uuid") << QString{};
        QTest::newRow("image-query") << QJsonValue(image + "?url=https://outside.example.invalid") << QString{};
        QTest::newRow("wrong-prefix-case") << QJsonValue("URN:image:aaaaaaaa-0000-4000-8000-000000000001") << QString{};
        QTest::newRow("other-source-urn") << QJsonValue("urn:url:https://images.example.invalid/avatar.png") << QString{};
    }
    void optionalImageAvatarReferenceSurvivesOwnedUsersCacheReopen()
    {
        QFETCH(QJsonValue, wireAvatar); QFETCH(QString, expected);
        QTemporaryDir directory; const NativeCache::Key users{NativeCache::Kind::Users, {}, {}, {}};
        QJsonObject person{{"uuid", User}, {"project_id", Project}, {"username", "avatar-self"},
            {"first_name", "Cassandra"}, {"email", "avatar-self@example.invalid"},
            {"credentials", QJsonObject{{"must_not_persist", "synthetic"}}}};
        if (!wireAvatar.isUndefined()) person["avatar"] = wireAvatar;
        {
            NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
            QVERIFY(cache.put(cache.lease(), users, {person}));
            const auto saved = cache.get(cache.lease(), users); QVERIFY(saved); QCOMPARE(saved->rows.size(), 1);
            QCOMPARE(saved->rows.first().toObject().value("username").toString(), QString("avatar-self"));
            QVERIFY(!saved->rows.first().toObject().contains("credentials"));
        }
        NativeCache reopened(directory.path()); QVERIFY(reopened.open(Server, Project, User));
        const auto saved = reopened.get(reopened.lease(), users); QVERIFY(saved); QVERIFY(!saved->fresh);
        const auto actual = saved->rows.first().toObject();
        QCOMPARE(actual.value("uuid").toString(), User); QCOMPARE(actual.value("first_name").toString(), QString("Cassandra"));
        QCOMPARE(actual.value("email").toString(), QString("avatar-self@example.invalid"));
        QVERIFY(!actual.contains("credentials"));
        if (expected.isEmpty()) QVERIFY(!actual.contains("avatar"));
        else QCOMPARE(actual.value("avatar").toString(), expected); // Old whitelist loses this actual witness.
        QVERIFY(reopened.diskUsageBytes() <= reopened.diskLimitBytes());
        QCOMPARE(reopened.diskLimitBytes(), NativeCache::DefaultDiskLimitBytes);
    }

    void imageAvatarReferenceReplacementNullAndForeignLeaseKeepTheSameOwnedUsersKey()
    {
        QTemporaryDir directory; NativeCache cache(directory.path());
        const NativeCache::Key users{NativeCache::Kind::Users, {}, {}, {}};
        const QString first = "urn:image:aaaaaaaa-0000-4000-8000-000000000001";
        const QString second = "urn:image:bbbbbbbb-0000-4000-8000-000000000002";
        QJsonObject person{{"uuid", User}, {"project_id", Project}, {"username", "avatar-self"}, {"avatar", first}};
        QVERIFY(cache.open(Server, Project, User)); const auto oldLease = cache.lease();
        QVERIFY(cache.put(oldLease, users, {person}));
        person["avatar"] = second; QVERIFY(cache.put(cache.lease(), users, {person}));
        QCOMPARE(cache.stats().entries, 1);
        const auto replacement = cache.get(cache.lease(), users); QVERIFY(replacement);
        QCOMPARE(replacement->rows.first().toObject().value("avatar").toString(), second);
        person["avatar"] = QJsonValue::Null; QVERIFY(cache.put(cache.lease(), users, {person}));
        const auto cleared = cache.get(cache.lease(), users); QVERIFY(cleared);
        QVERIFY(!cleared->rows.first().toObject().contains("avatar"));
        QVERIFY(cache.open(Server, Project, UserB));
        person["avatar"] = second; QVERIFY(cache.put(cache.lease(), users, {person}));
        person["avatar"] = first; QVERIFY(!cache.put(oldLease, users, {person}));
        const auto other = cache.get(cache.lease(), users); QVERIFY(other);
        QCOMPARE(other->rows.first().toObject().value("avatar").toString(), second);
        QVERIFY(cache.open(Server, Project, User));
        const auto prior = cache.get(cache.lease(), users); QVERIFY(prior);
        QVERIFY(!prior->rows.first().toObject().contains("avatar")); // Other owner did not replace this cleared reference.
    }

    // Separate compatibility supplement. Missing raw viewer is allowed only for
    // a typed partial mask over the already-owned same-route saved body.
    void ownedBodyPartialMasksInheritViewerWithoutOwningPlacement_data()
    {
        QTest::addColumn<int>("fields");
        QTest::newRow("star") << int(NativeCache::Projection::Star);
        QTest::newRow("reactions") << int(NativeCache::Projection::Reactions);
    }
    void ownedBodyPartialMasksInheritViewerWithoutOwningPlacement()
    {
        QFETCH(int, fields); QTemporaryDir directory; qint64 now = 10000;
        NativeCache cache(directory.path(), {}, false, [&] { return now; }); QVERIFY(cache.open(Server, Project, User));
        auto row = message("Original retained payload"); row["user_uuid"] = User; row["read"] = true;
        row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        const auto id = row.value("uuid").toString();
        QVERIFY(cache.putMessageBody(cache.lease(), row)); QVERIFY(cache.put(cache.lease(), History, {row}, "before"));
        const auto original = cache.getMessageBody(cache.lease(), id); QVERIFY(original); now = 20000;
        auto partial = row; partial.remove("user_uuid"); partial["read"] = false; partial["starred"] = true;
        partial["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Partial response does not own payload"}};
        partial["reactions"] = QJsonObject{{"heart", 1}}; partial["reaction_users"] = QJsonObject{{"heart", QJsonArray{User}}};
        QVERIFY(cache.projectMessage(cache.lease(), partial, NativeCache::ProjectionFields::fromInt(fields)));
        auto expected = row;
        if (fields == int(NativeCache::Projection::Star)) expected["starred"] = true;
        else { expected["reactions"] = partial["reactions"]; expected["reaction_users"] = partial["reaction_users"]; }
        const auto saved = cache.getMessageBody(cache.lease(), id); QVERIFY(saved);
        QCOMPARE(saved->row, expected); QCOMPARE(saved->savedAt, original->savedAt);
        const auto page = cache.get(cache.lease(), History); QVERIFY(page); QCOMPARE(page->rows, QJsonArray{expected});
        QCOMPARE(page->savedAt, original->savedAt); QCOMPARE(page->nextMarker, QByteArray("before")); QVERIFY(!page->fresh);
        auto wrongRoute = partial; wrongRoute["stream_uuid"] = UserB; wrongRoute["topic_uuid"] = User;
        QVERIFY(cache.projectMessage(cache.lease(), wrongRoute, NativeCache::ProjectionFields::fromInt(fields)));
        QCOMPARE(cache.getMessageBody(cache.lease(), id)->row, expected); // Partial facts cannot move UUID.
        auto foreign = partial; foreign["user_uuid"] = UserB;
        QVERIFY(!cache.projectMessage(cache.lease(), foreign, NativeCache::ProjectionFields::fromInt(fields)));
        QVERIFY(!cache.projectMessage(cache.lease(), partial, NativeCache::Projection::Payload));
        QCOMPARE(cache.getMessageBody(cache.lease(), id)->row, expected);
        QCOMPARE(cache.get(cache.lease(), History)->rows, QJsonArray{expected});
    }

    // Independent supplement: preserve QTEST_GUILESS_MAIN. Each row forks only
    // this same slot into a fresh process, so earlier Qt allocations cannot mask
    // the read's peak. Linux /proc values are numeric public test evidence.
    void corruptOversizedBodyReadsDoNotMaterializeTheBlob_data()
    {
        QTest::addColumn<bool>("historyFallback");
        QTest::newRow("direct-body") << false;
        QTest::newRow("indexed-history-fallback") << true;
    }
    void corruptOversizedBodyReadsDoNotMaterializeTheBlob()
    {
#ifndef Q_OS_LINUX
        QSKIP("The isolated VmHWM witness is Linux-specific.");
#else
        QFETCH(bool, historyFallback);
        const QString id = "55555555-5555-4555-8555-000000000001";
        const QString retainedId = "55555555-5555-4555-8555-000000000002";
        const auto childDirectory = qEnvironmentVariable("CASSI_NATIVE_BODY_BLOB_CHILD_DIRECTORY");
        if (!childDirectory.isEmpty()) {
            NativeCache cache(childDirectory); QVERIFY(cache.open(Server, Project, User));
            const auto highWaterKiB = []() -> qint64 {
                QFile status("/proc/self/status"); if (!status.open(QIODevice::ReadOnly)) return -1;
                for (const auto &raw : status.readAll().split('\n')) {
                    const auto line = raw.simplified();
                    if (line.startsWith("VmHWM:")) return line.split(' ').value(1).toLongLong();
                }
                return -1;
            };
            const auto before = highWaterKiB(); QVERIFY(before > 0);
            QVERIFY(!cache.getMessageBody(cache.lease(), id));
            const auto after = highWaterKiB(); QVERIFY(after >= before);
            QVERIFY(cache.available()); // Corrupt row is a miss; unrelated valid data survives.
            const auto retained = cache.getMessageBody(cache.lease(), retainedId); QVERIFY(retained);
            QCOMPARE(retained->row.value("uuid").toString(), retainedId);
            qInfo("body-read-vmhwm beforeKiB=%lld afterKiB=%lld deltaKiB=%lld fallback=%d",
                static_cast<long long>(before), static_cast<long long>(after),
                static_cast<long long>(after - before), int(historyFallback));
            QVERIFY2(after - before < 16 * 1024, "A rejected 64 MiB BLOB must not be copied into the Qt result.");
            return;
        }
        QTemporaryDir directory;
        auto row = message("Will be replaced with zeroblob"); row["uuid"] = id; row["user_uuid"] = User;
        auto retained = message("Retained bounded row"); retained["uuid"] = retainedId; retained["user_uuid"] = User;
        QString owner;
        {
            NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User)); owner = cache.lease().owner;
            if (historyFallback) QVERIFY(cache.put(cache.lease(), History, {row}, "retained-cursor"));
            else QVERIFY(cache.putMessageBody(cache.lease(), row));
            QVERIFY(cache.putMessageBody(cache.lease(), retained));
        }
        const auto connection = QUuid::createUuid().toString();
        const auto removeConnection = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(db.open());
            {
                QSqlQuery query(db); query.prepare("UPDATE entries SET payload=zeroblob(67108864) WHERE owner=? AND key IN"
                    " (SELECT key FROM history_rows WHERE owner=? AND message_uuid=?)");
                query.addBindValue(owner); query.addBindValue(owner); query.addBindValue(id);
                QVERIFY(query.exec()); QCOMPARE(query.numRowsAffected(), qint64(1));
                query.finish();
                query.prepare("SELECT LENGTH(payload) FROM entries WHERE owner=? AND key IN"
                    " (SELECT key FROM history_rows WHERE owner=? AND message_uuid=?)");
                query.addBindValue(owner); query.addBindValue(owner); query.addBindValue(id);
                QVERIFY(query.exec()); QVERIFY(query.next()); QCOMPARE(query.value(0).toLongLong(), qint64(67108864));
                query.finish(); QVERIFY(query.exec("PRAGMA wal_checkpoint(TRUNCATE)")); query.finish();
            }
            db.close();
        }
        QProcess child; auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CASSI_NATIVE_BODY_BLOB_CHILD_DIRECTORY", directory.path()); child.setProcessEnvironment(environment);
        const auto reap = qScopeGuard([&] { if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(2000); } });
        child.start(QCoreApplication::applicationFilePath(), {QString::fromLatin1(QTest::currentTestFunction()) + ":" + QTest::currentDataTag()});
        QVERIFY2(child.waitForStarted(2000), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(10000), "Isolated bounded-read witness did not finish.");
        const auto evidence = child.readAllStandardOutput() + child.readAllStandardError();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit, evidence.constData());
        QVERIFY2(child.exitCode() == 0, evidence.constData());
#endif
    }

    // Additive capability/regression slots; SOURCE-ONLY, NOT RUN.
    void messageBodyIsOwnedTypedSingleRecordAndSurvivesReopen()
    {
        QTemporaryDir directory; NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto row = message("Body only"); row["user_uuid"] = User;
        row["payload"] = QJsonObject{{"kind", "key_request"}, {"key_uuid", Topic},
            {"device_id", "synthetic-device"}, {"device_name", "Synthetic device"},
            {"public_key", QString::fromLatin1(QByteArray(32, 'p').toBase64())}};
        row["access_token"] = "must-not-be-cached";
        QVERIFY(cache.putMessageBody(lease, row));
        const auto id = row.value("uuid").toString();
        auto body = cache.getMessageBody(lease, id); QVERIFY(body);
        QVERIFY(!body->row.contains("access_token")); QCOMPARE(body->row.value("user_uuid").toString(), User);
        QCOMPARE(body->row.value("payload"), row.value("payload"));
        QCOMPARE(body->row.value("created_at"), row.value("created_at"));
        QVERIFY(!cache.get(lease, History)); // Body never creates a History page.
        const auto savedAt = body->savedAt; cache.close();
        QVERIFY(cache.open(Server, Project, User)); body = cache.getMessageBody(cache.lease(), id); QVERIFY(body);
        QCOMPARE(body->savedAt, savedAt); QCOMPARE(body->row.value("payload"), row.value("payload"));
        const auto original = body->row;
        for (const auto &field : {QString("user_uuid"), QString("project_id")}) {
            auto invalid = row; invalid[field] = UserB;
            QVERIFY(!cache.putMessageBody(cache.lease(), invalid));
        }
        auto missingViewer = row; missingViewer.remove("user_uuid");
        QVERIFY(!cache.putMessageBody(cache.lease(), missingViewer));
        const NativeCache::Key wrongId{NativeCache::Kind::MessageBody, Stream, Topic, UserB.toUtf8()};
        QVERIFY(!cache.put(cache.lease(), wrongId, {row}));
        QCOMPARE(cache.getMessageBody(cache.lease(), id)->row, original);
        QVERIFY(!cache.getMessageBody(lease, id)); // Pre-reopen lease is invalid.
        QVERIFY(cache.open(Server, Project, UserB)); QVERIFY(!cache.getMessageBody(cache.lease(), id));
        auto other = row; other["user_uuid"] = UserB;
        QVERIFY(cache.putMessageBody(cache.lease(), other));
        QVERIFY(cache.open(Server, Project, User));
        QCOMPARE(cache.getMessageBody(cache.lease(), id)->row, original);
    }

    void messageBodyFallbackRequiresViewerIdentityAndNeverManufacturesHistory()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = message("Prior History body"); const auto id = row.value("uuid").toString();
        QVERIFY(cache.put(cache.lease(), History, {row}, "older"));
        QVERIFY(!cache.getMessageBody(cache.lease(), id)); // Legacy trimmed page has no verified raw viewer.
        row["user_uuid"] = User; QVERIFY(cache.put(cache.lease(), History, {row}, "older"));
        const auto page = cache.get(cache.lease(), History); QVERIFY(page);
        const auto body = cache.getMessageBody(cache.lease(), id); QVERIFY(body);
        QCOMPARE(body->row, row); QCOMPARE(body->savedAt, page->savedAt);
        const NativeCache::Key direct{NativeCache::Kind::MessageBody, {}, {}, id.toUtf8()};
        QVERIFY(!cache.get(cache.lease(), direct)); // Read fallback does not synthesize another disk entry.
        QCOMPARE(cache.get(cache.lease(), History)->nextMarker, QByteArray("older"));
        QCOMPARE(cache.get(cache.lease(), History)->savedAt, page->savedAt);
    }

    void messageBodyHistoryFallbackReadsAtMostFourIndexedCandidates()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = message("Fifth owned candidate"); row["user_uuid"] = User;
        const auto id = row.value("uuid").toString();
        for (int n = 0; n < 5; ++n) { auto key = History; key.marker = QByteArray::number(n); QVERIFY(cache.put(cache.lease(), key, {row})); }
        const auto connection = QUuid::createUuid().toString();
        const auto removeConnection = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(db.open());
            QList<QByteArray> keys;
            { QSqlQuery q(db); q.prepare("SELECT key FROM history_rows INDEXED BY cache_message_lookup WHERE owner=? AND message_uuid=? ORDER BY key");
              q.addBindValue(cache.lease().owner); q.addBindValue(id); QVERIFY(q.exec()); while (q.next()) keys.append(q.value(0).toByteArray()); }
            QCOMPARE(keys.size(), 5);
            auto foreign = row; foreign["user_uuid"] = UserB;
            const auto payload = QJsonDocument(QJsonArray{foreign}).toJson(QJsonDocument::Compact);
            { QSqlQuery q(db); q.prepare("UPDATE entries SET payload=? WHERE owner=? AND key=?");
              for (int n = 0; n < 4; ++n) { q.bindValue(0, payload); q.bindValue(1, cache.lease().owner); q.bindValue(2, keys[n]); QVERIFY(q.exec()); } }
            QVERIFY(!cache.getMessageBody(cache.lease(), id)); // A fifth valid body may not widen the bound.
            { QSqlQuery q(db); QVERIFY(q.exec("PRAGMA foreign_keys=ON")); q.prepare("DELETE FROM entries WHERE owner=? AND key=?");
              q.addBindValue(cache.lease().owner); q.addBindValue(keys.first()); QVERIFY(q.exec()); }
            const auto body = cache.getMessageBody(cache.lease(), id); QVERIFY(body); QCOMPARE(body->row, row);
            db.close();
        }
    }

    void movedIndexedHistoryRetainsBodyOutsideDestinationWindow_data()
    {
        QTest::addColumn<bool>("destinationWindow");
        QTest::addColumn<bool>("standaloneBody");
        QTest::newRow("no-destination-window") << false << false;
        QTest::newRow("outside-older-bound") << true << false;
        QTest::newRow("existing-body-control") << true << true;
    }

    void movedIndexedHistoryRetainsBodyOutsideDestinationWindow()
    {
        QFETCH(bool, destinationWindow); QFETCH(bool, standaloneBody);
        QTemporaryDir directory; NativeCache cache(directory.path());
        auto original = message("Known only in history"); original["user_uuid"] = User; original["read"] = true;
        const auto id = original.value("uuid").toString();
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        const NativeCache::Key sourceAggregate{NativeCache::Kind::History, Stream, {}, {}};
        const NativeCache::Key destination{NativeCache::Kind::History, UserB, User, {}};
        const NativeCache::Key destinationAggregate{NativeCache::Kind::History, UserB, {}, {}};
        const NativeCache::Key exactBody{NativeCache::Kind::MessageBody, {}, {}, id.toLower().toUtf8()};
        QVERIFY(cache.put(lease, History, {original}, "source-older"));
        QVERIFY(cache.put(lease, sourceAggregate, {original}, "aggregate-older"));
        if (standaloneBody) QVERIFY(cache.putMessageBody(lease, original));
        QCOMPARE(cache.get(lease, exactBody).has_value(), standaloneBody);
        const auto previouslyReadable = cache.getMessageBody(lease, id); QVERIFY(previouslyReadable);
        QCOMPARE(previouslyReadable->row, original);
        auto latest = original; latest["uuid"] = "88888888-8888-4888-8888-000000000099";
        latest["stream_uuid"] = UserB; latest["topic_uuid"] = User;
        latest["created_at"] = "2026-11-06T12:35:56.123456Z";
        if (destinationWindow) QVERIFY(cache.put(lease, destination, {latest}, "destination-older"));
        auto moved = original; moved["stream_uuid"] = UserB; moved["topic_uuid"] = User;
        moved["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Canonical moved body"}};
        QVERIFY(cache.projectMessage(lease, moved, NativeCache::Projection::Insert | NativeCache::Projection::Payload | NativeCache::Projection::Star));
        const auto source = cache.get(lease, History); QVERIFY(source); QVERIFY(source->rows.isEmpty());
        QCOMPARE(source->nextMarker, QByteArray("source-older")); QVERIFY(!source->fresh);
        const auto aggregate = cache.get(lease, sourceAggregate); QVERIFY(aggregate); QVERIFY(aggregate->rows.isEmpty());
        const auto target = cache.get(lease, destination);
        QCOMPARE(target.has_value(), destinationWindow);
        if (target) { QCOMPARE(target->rows, QJsonArray{latest}); QCOMPARE(target->nextMarker, QByteArray("destination-older")); }
        QVERIFY(!cache.get(lease, destinationAggregate)); // No invented destination window.
        const auto retained = cache.getMessageBody(lease, id);
        QVERIFY2(retained.has_value(), "A moved known body must remain UUID-readable even outside every destination window");
        QCOMPARE(retained->row, moved);
        // Reopening proves this is retained on disk, not an in-memory copy.
        cache.close(); QVERIFY(cache.open(Server, Project, User));
        const auto reopened = cache.getMessageBody(cache.lease(), id); QVERIFY(reopened);
        QCOMPARE(reopened->row, moved);
        QVERIFY(cache.removeMessageBodyAndHistory(cache.lease(), id));
        QVERIFY(cache.projectMessage(cache.lease(), moved, NativeCache::Projection::Insert | NativeCache::Projection::Payload));
        QVERIFY(!cache.getMessageBody(cache.lease(), id)); // No resurrection after actual erasure.
    }

    void movedHistoryBodyRetentionKeepsOwnershipMasksAndExistingAge_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"new-owned", "existing-body", "partial-mask", "foreign-history-viewer", "foreign-history-owned-dto",
                                "foreign-dto-viewer", "legacy-dto-without-viewer", "erased-before-move", "unknown-uuid",
                                "entry-pressure"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void movedHistoryBodyRetentionKeepsOwnershipMasksAndExistingAge()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory; qint64 now = 10000;
        NativeCache::Policy policy; policy.maxEntries = scenario == "entry-pressure" ? 2 : 0;
        NativeCache cache(directory.path(), policy, false, [&] { return now; });
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto original = message("Previously indexed body"); original["user_uuid"] = scenario.startsWith("foreign-history") ? UserB : User;
        original["read"] = true; original["starred"] = true;
        original["reactions"] = QJsonObject{{QString::fromUtf8("👍"), 1}};
        const auto id = original.value("uuid").toString();
        const NativeCache::Key body{NativeCache::Kind::MessageBody, {}, {}, id.toUtf8()};
        if (scenario != "unknown-uuid") QVERIFY(cache.put(lease, History, {original}, "source-older"));
        if (scenario == "entry-pressure")
            QVERIFY(cache.put(lease, {NativeCache::Kind::History, Stream, {}, {}}, {original}, "aggregate-older"));
        if (scenario == "existing-body") QVERIFY(cache.putMessageBody(lease, original));
        QCOMPARE(cache.get(lease, body).has_value(), scenario == "existing-body");
        if (scenario == "erased-before-move") QVERIFY(cache.removeMessageBodyAndHistory(lease, id));
        now += 5000; // A new Body may record this canonical event; an existing Body must keep its age.
        auto moved = original; moved["stream_uuid"] = UserB; moved["topic_uuid"] = User;
        moved["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Canonical moved body"}};
        moved["read"] = false; moved["starred"] = false;
        moved["reactions"] = QJsonObject{{QString::fromUtf8("👍"), 9}};
        if (scenario == "foreign-history-owned-dto") moved["user_uuid"] = User;
        if (scenario == "foreign-dto-viewer") moved["user_uuid"] = UserB;
        if (scenario == "legacy-dto-without-viewer") moved.remove("user_uuid");
        const auto fields = scenario == "partial-mask" ? NativeCache::ProjectionFields(NativeCache::Projection::Star)
            : NativeCache::Projection::Insert | NativeCache::Projection::Payload;
        QVERIFY(cache.projectMessage(lease, moved, fields));
        const bool retained = scenario == "new-owned" || scenario == "existing-body" || scenario == "entry-pressure";
        const auto saved = cache.get(lease, body); QCOMPARE(saved.has_value(), retained);
        if (saved) {
            auto expected = original; expected["stream_uuid"] = UserB; expected["topic_uuid"] = User;
            expected["payload"] = moved.value("payload");
            QCOMPARE(saved->rows, QJsonArray{expected}); // Read is monotonic; unmasked star/reactions are preserved.
            QCOMPARE(saved->savedAt, scenario == "existing-body" ? qint64(10000) : now);
        }
        if (const auto page = cache.get(lease, History)) {
            QCOMPARE(page->savedAt, qint64(10000)); QCOMPARE(page->nextMarker, QByteArray("source-older"));
            QCOMPARE(page->rows, scenario == "partial-mask" ? QJsonArray{original} : QJsonArray{});
        }
        if (scenario == "entry-pressure") QVERIFY(cache.stats().entries <= 2);
    }

    void movedHistoryBodyUsesNewestUnmaskedFieldsAndAllKnownReadFlags_data()
    {
        QTest::addColumn<bool>("reverseKeys");
        QTest::newRow("first-key-newer") << false;
        QTest::newRow("second-key-newer") << true;
    }

    void movedHistoryBodyUsesNewestUnmaskedFieldsAndAllKnownReadFlags()
    {
        QFETCH(bool, reverseKeys);
        QTemporaryDir directory; qint64 now = 10000;
        NativeCache cache(directory.path(), {}, false, [&] { return now; });
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto older = message("Older cached body"); older["user_uuid"] = User; older["read"] = true;
        older["reactions"] = QJsonObject{{QString::fromUtf8("👍"), 1}};
        auto newer = older; newer["read"] = false; newer["starred"] = true;
        newer["reactions"] = QJsonObject{{QString::fromUtf8("👍"), 2}};
        auto oldKey = History, newKey = History;
        oldKey.marker = reverseKeys ? "a" : "b"; newKey.marker = reverseKeys ? "b" : "a";
        QVERIFY(cache.put(lease, oldKey, {older}, "older-cursor"));
        now = 20000; QVERIFY(cache.put(lease, newKey, {newer}, "newer-cursor"));
        const auto id = older.value("uuid").toString();
        QVERIFY(!cache.get(lease, {NativeCache::Kind::MessageBody, {}, {}, id.toUtf8()}));
        now = 30000;
        auto moved = older; moved["stream_uuid"] = UserB; moved["topic_uuid"] = User;
        moved["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Canonical moved body"}};
        moved["read"] = false; moved["starred"] = false;
        moved["reactions"] = QJsonObject{{QString::fromUtf8("👍"), 9}};
        QVERIFY(cache.projectMessage(lease, moved, NativeCache::Projection::Insert | NativeCache::Projection::Payload));
        auto expected = newer; expected["stream_uuid"] = UserB; expected["topic_uuid"] = User;
        expected["payload"] = moved.value("payload"); expected["read"] = true;
        const auto retained = cache.getMessageBody(lease, id); QVERIFY(retained);
        QCOMPARE(retained->row, expected); QCOMPARE(retained->savedAt, now);
        for (const auto &entry : {std::make_pair(oldKey, qint64(10000)), std::make_pair(newKey, qint64(20000))}) {
            const auto page = cache.get(lease, entry.first); QVERIFY(page); QVERIFY(page->rows.isEmpty());
            QCOMPARE(page->savedAt, entry.second); QVERIFY(!page->fresh);
            QCOMPARE(page->nextMarker, entry.second == 10000 ? QByteArray("older-cursor") : QByteArray("newer-cursor"));
        }
        cache.close(); QVERIFY(cache.open(Server, Project, User));
        const auto reopened = cache.getMessageBody(cache.lease(), id); QVERIFY(reopened);
        QCOMPARE(reopened->row, expected); QCOMPARE(reopened->savedAt, now);
    }

    void declinedMovedBodyCannotBypassTheEntryBudget()
    {
        QTemporaryDir directory; NativeCache::Policy policy; policy.maxEntryBytes = 2048;
        NativeCache cache(directory.path(), policy, false);
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto original = message("Known small body"); original["user_uuid"] = User;
        QVERIFY(cache.put(lease, History, {original}, "source-older"));
        auto moved = original; moved["stream_uuid"] = UserB; moved["topic_uuid"] = User;
        moved["payload"] = QJsonObject{{"kind", "markdown"}, {"content", QString(4096, 'x')}};
        QVERIFY(!cache.projectMessage(lease, moved, NativeCache::Projection::Insert | NativeCache::Projection::Payload));
        const auto id = original.value("uuid").toString();
        QVERIFY(!cache.getMessageBody(lease, id)); QVERIFY(cache.available());
        const auto source = cache.get(lease, History); QVERIFY(source); QVERIFY(source->rows.isEmpty());
        QCOMPARE(source->nextMarker, QByteArray("source-older")); QVERIFY(!source->fresh);
        QVERIFY(!cache.get(lease, {NativeCache::Kind::History, UserB, User, {}}));
        QVERIFY(cache.stats().bytes <= policy.maxEntryBytes);
        cache.close(); QVERIFY(cache.open(Server, Project, User));
        QVERIFY(!cache.getMessageBody(cache.lease(), id));
    }

    void messageBodyProjectionOwnsFullPlacementButNotPartialPlacementOrAge()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = message("Before move"); row["user_uuid"] = User; row["read"] = true;
        const auto id = row.value("uuid").toString();
        QVERIFY(cache.putMessageBody(cache.lease(), row)); QVERIFY(cache.put(cache.lease(), History, {row}, "before"));
        const auto original = cache.getMessageBody(cache.lease(), id); QVERIFY(original);
        auto moved = row; moved["stream_uuid"] = UserB; moved["topic_uuid"] = User; moved["read"] = false;
        moved["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "After move"}};
        QVERIFY(cache.projectMessage(cache.lease(), moved, NativeCache::Projection::Payload));
        const auto saved = cache.getMessageBody(cache.lease(), id); QVERIFY(saved);
        QCOMPARE(saved->row.value("stream_uuid").toString(), UserB); QCOMPARE(saved->row.value("topic_uuid").toString(), User);
        QCOMPARE(saved->row.value("payload"), moved.value("payload")); QVERIFY(saved->row.value("read").toBool());
        QCOMPARE(saved->savedAt, original->savedAt); QVERIFY(cache.get(cache.lease(), History)->rows.isEmpty());
        auto partial = row; partial["starred"] = true;
        QVERIFY(cache.projectMessage(cache.lease(), partial, NativeCache::Projection::Star));
        QCOMPARE(cache.getMessageBody(cache.lease(), id)->row, saved->row);
        auto wrongViewer = moved; wrongViewer["user_uuid"] = UserB;
        QVERIFY(!cache.projectMessage(cache.lease(), wrongViewer, NativeCache::Projection::Payload));
        QCOMPARE(cache.getMessageBody(cache.lease(), id)->row, saved->row);
        QVERIFY(cache.markHistoryRead(cache.lease(), UserB, User, {id}));
        QVERIFY(cache.getMessageBody(cache.lease(), id)->row.value("read").toBool());
        QVERIFY(cache.invalidate(cache.lease(), NativeCache::Kind::History, UserB, User));
        QVERIFY(!cache.getMessageBody(cache.lease(), id));
        QVERIFY(cache.projectMessage(cache.lease(), moved, NativeCache::Projection::Insert | NativeCache::Projection::Payload));
        QVERIFY(!cache.getMessageBody(cache.lease(), id)); // Projection never resurrects an erased Body.
    }

    void authoritativeBodyOmissionErasesEveryIndexedCopyOnlyForItsOwner()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); auto row = message("Removed UUID"); row["user_uuid"] = User;
        const auto id = row.value("uuid").toString(); auto foreign = row; foreign["user_uuid"] = UserB;
        QVERIFY(cache.open(Server, Project, UserB)); QVERIFY(cache.putMessageBody(cache.lease(), foreign));
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QVERIFY(cache.putMessageBody(lease, row)); QVERIFY(cache.put(lease, History, {row}, "topic-older"));
        auto older = History; older.marker = "older"; QVERIFY(cache.put(lease, older, {row}, "next"));
        const NativeCache::Key aggregate{NativeCache::Kind::History, Stream, {}, {}}; QVERIFY(cache.put(lease, aggregate, {row}));
        auto unrelated = message("Retained UUID"); unrelated["user_uuid"] = User;
        const NativeCache::Key other{NativeCache::Kind::History, Stream, UserB, {}}; unrelated["topic_uuid"] = UserB;
        QVERIFY(cache.put(lease, other, {unrelated}));
        QVERIFY(cache.removeMessageBodyAndHistory(lease, id)); QVERIFY(!cache.getMessageBody(lease, id));
        QVERIFY(!cache.get(lease, History)); QVERIFY(!cache.get(lease, older)); QVERIFY(!cache.get(lease, aggregate));
        QVERIFY(cache.get(lease, other));
        cache.close(); QVERIFY(cache.open(Server, Project, User)); QVERIFY(!cache.getMessageBody(cache.lease(), id));
        QVERIFY(cache.open(Server, Project, UserB)); QVERIFY(cache.getMessageBody(cache.lease(), id));
    }

    void messageBodySharesLruEntryAndPhysicalDiskBudgets()
    {
        QTemporaryDir directory; NativeCache::Policy policy; policy.maxEntries = 2; policy.maxEntryBytes = 4096;
        NativeCache cache(directory.path(), policy, false); QVERIFY(cache.open(Server, Project, User));
        QStringList ids;
        for (int n = 0; n < 3; ++n) { auto row = message(QString::number(n)); row["user_uuid"] = User; ids.append(row.value("uuid").toString()); QVERIFY(cache.putMessageBody(cache.lease(), row)); }
        QVERIFY(!cache.getMessageBody(cache.lease(), ids.first())); QVERIFY(cache.getMessageBody(cache.lease(), ids.last()));
        QCOMPARE(cache.stats().entries, qint64(2));
        auto large = message(QString(5000, 'x')); large["user_uuid"] = User; QVERIFY(!cache.putMessageBody(cache.lease(), large));
        QVERIFY(cache.getMessageBody(cache.lease(), ids.last()));
        QTemporaryDir physical; NativeCache bounded(physical.path()); QVERIFY(bounded.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
        QVERIFY(bounded.open(Server, Project, User)); QString latest;
        for (int n = 0; n < 100; ++n) { auto row = message(QString(8192, 'x')); row["user_uuid"] = User; latest = row.value("uuid").toString();
            QVERIFY(bounded.putMessageBody(bounded.lease(), row)); QVERIFY(bounded.diskUsageBytes() <= NativeCache::MinimumDiskLimitBytes); }
        QVERIFY(bounded.getMessageBody(bounded.lease(), latest));
    }

    void messageBodySchemaFourMigrationPreservesOldKeysAndPages_data()
    {
        QTest::addColumn<int>("version"); for (int v : {1,2,3}) QTest::newRow(qPrintable(QString::number(v))) << v;
    }
    void messageBodySchemaFourMigrationPreservesOldKeysAndPages()
    {
        QFETCH(int, version); QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto owner = cache.lease().owner; auto row = message("Legacy retained");
        QVERIFY(cache.put(cache.lease(), Streams, {stream("Legacy")})); QVERIFY(cache.put(cache.lease(), History, {row}, "legacy-cursor"));
        const auto connection = QUuid::createUuid().toString(); QList<QByteArray> oldKeys;
        const auto removeConnection = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
        { auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(db.open());
          { QSqlQuery q(db); QVERIFY(q.exec("SELECT key FROM entries ORDER BY key")); while (q.next()) oldKeys.append(q.value(0).toByteArray()); } db.close(); }
        cache.close();
        if (version == 1) QVERIFY(modify(directory.path(), "DROP TABLE history_rows"));
        QVERIFY(modify(directory.path(), "PRAGMA user_version=" + QString::number(version)));
        QVERIFY(cache.open(Server, Project, User)); QCOMPARE(cache.lease().owner, owner);
        QCOMPARE(cache.get(cache.lease(), History)->rows, QJsonArray{row}); QCOMPARE(cache.get(cache.lease(), History)->nextMarker, QByteArray("legacy-cursor"));
        QVERIFY(!cache.getMessageBody(cache.lease(), row.value("uuid").toString()));
        const auto inspect = QUuid::createUuid().toString();
        const auto removeInspect = qScopeGuard([&] { QSqlDatabase::removeDatabase(inspect); });
        { auto db = QSqlDatabase::addDatabase("QSQLITE", inspect); db.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(db.open());
          { QSqlQuery q(db); QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 4);
            QVERIFY(q.exec("SELECT key FROM entries ORDER BY key")); QList<QByteArray> keys; while (q.next()) keys.append(q.value(0).toByteArray()); QCOMPARE(keys, oldKeys); } db.close(); }
    }

    void messageBodyFutureSemanticVersionFailsWithoutChangingTheFile()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = message(); row["user_uuid"] = User; QVERIFY(cache.putMessageBody(cache.lease(), row)); cache.close();
        QVERIFY(modify(directory.path(), "PRAGMA user_version=5"));
        QFile file(directory.filePath("cache.sqlite3")); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll(); file.close();
        QVERIFY(!cache.open(Server, Project, User)); QVERIFY(!cache.available());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
    }

    void placementInsertionRespectsSavedOlderBoundary_data() {
        QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("hasOlder"); QTest::addColumn<bool>("inserted");
        QTest::newRow("older-bounded")<<QString("older")<<true<<false;
        QTest::newRow("older-complete")<<QString("older")<<false<<true;
        QTest::newRow("same-time-lower-id")<<QString("lower")<<true<<false;
        QTest::newRow("same-time-higher-id")<<QString("higher")<<true<<true;
        QTest::newRow("newer-bounded")<<QString("newer")<<true<<true;
        QTest::newRow("empty-bounded")<<QString("empty")<<true<<false;
        QTest::newRow("empty-complete")<<QString("empty")<<false<<true;
    }
    void placementInsertionRespectsSavedOlderBoundary() {
        QFETCH(QString,scenario); QFETCH(bool,hasOlder); QFETCH(bool,inserted);
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server,Project,User));
        auto existing=message("Already loaded"); existing["uuid"]="88888888-8888-4888-8888-000000000020";
        auto incoming=message("Moved into this conversation"); incoming["uuid"]="88888888-8888-4888-8888-000000000030";
        if(scenario=="older") incoming["created_at"]="2026-11-06T12:34:55.123456Z";
        if(scenario=="newer") incoming["created_at"]="2026-11-06T12:34:57.123456Z";
        if(scenario=="lower") incoming["uuid"]="88888888-8888-4888-8888-000000000010";
        const QJsonArray original=scenario=="empty"?QJsonArray{}:QJsonArray{existing};
        const QByteArray next=hasOlder?QByteArray("older-cursor"):QByteArray{};
        const auto lease=cache.lease(); QVERIFY(cache.put(lease,History,original,next));
        const auto before=cache.get(lease,History); QVERIFY(before);
        QVERIFY(cache.projectMessage(lease,incoming,NativeCache::Projection::Insert|NativeCache::Projection::Payload));
        const auto saved=cache.get(lease,History); QVERIFY(saved); QCOMPARE(saved->nextMarker,next);
        QCOMPARE(saved->savedAt,before->savedAt); QVERIFY(!saved->fresh);
        bool contains=false; for(const auto &value:saved->rows) contains |= value.toObject().value("uuid")==incoming.value("uuid");
        QCOMPARE(contains,inserted); QCOMPARE(saved->rows.size(),original.size()+(inserted?1:0));
        cache.close(); QVERIFY(cache.open(Server,Project,User));
        QCOMPARE(cache.get(cache.lease(),History)->rows,saved->rows);
    }
    void knownPlacementProjectionMovesSavedMembership_data() {
        QTest::addColumn<bool>("crossStream"); QTest::addColumn<bool>("alreadyRead");
        QTest::newRow("same-stream")<<false<<false; QTest::newRow("cross-stream")<<true<<false;
        QTest::newRow("same-stream-read")<<false<<true; QTest::newRow("cross-stream-read")<<true<<true;
    }
    void knownPlacementProjectionMovesSavedMembership() {
        QFETCH(bool,crossStream); QFETCH(bool,alreadyRead); QTemporaryDir directory; NativeCache cache(directory.path());
        auto row=message("Original body"); row["read"]=alreadyRead;
        QVERIFY(cache.open(Server,Project,UserB)); QVERIFY(cache.put(cache.lease(),History,{row},"foreign-cursor"));
        QVERIFY(cache.open(Server,Project,User)); const auto lease=cache.lease();
        const QString targetStream=crossStream?UserB:Stream;
        const NativeCache::Key sourceAggregate{NativeCache::Kind::History,Stream,{},{}};
        const NativeCache::Key destination{NativeCache::Kind::History,targetStream,UserB,{}};
        const NativeCache::Key destinationAggregate{NativeCache::Kind::History,targetStream,{},{}};
        QVERIFY(cache.put(lease,History,{row},"source-older")); QVERIFY(cache.put(lease,sourceAggregate,{row},"aggregate-older"));
        QVERIFY(cache.put(lease,destination,{})); if(crossStream)QVERIFY(cache.put(lease,destinationAggregate,{}));
        const auto originalRow=row; row["stream_uuid"]=targetStream; row["topic_uuid"]=UserB; row["read"]=false;
        QVERIFY(cache.projectMessage(lease,row,NativeCache::Projection::Insert|NativeCache::Projection::Payload|NativeCache::Projection::Star));
        row["read"]=alreadyRead;
        const auto source=cache.get(lease,History); QVERIFY(source);
        QVERIFY2(source->rows.isEmpty(),"The cache retained a moved UUID in its former topic page");
        QCOMPARE(source->nextMarker,QByteArray("source-older")); QVERIFY(!source->fresh);
        const auto target=cache.get(lease,destination); QVERIFY(target); QCOMPARE(target->rows,QJsonArray{row});
        const auto aggregate=cache.get(lease,sourceAggregate); QVERIFY(aggregate);
        QCOMPARE(aggregate->rows,crossStream?QJsonArray{}:QJsonArray{row});
        cache.close(); QVERIFY(cache.open(Server,Project,User));
        QVERIFY(cache.get(cache.lease(),History)->rows.isEmpty());
        QCOMPARE(cache.get(cache.lease(),destination)->rows,QJsonArray{row});
        cache.close(); QVERIFY(cache.open(Server,Project,UserB));
        const auto foreign=cache.get(cache.lease(),History); QVERIFY(foreign);
        QCOMPARE(foreign->rows,QJsonArray{originalRow}); QCOMPARE(foreign->nextMarker,QByteArray("foreign-cursor"));
    }
    void archivedTimestampSurvivesRestartWithoutChangingCatalogKind() {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = stream("Archived"); row["is_archived"] = true; row["updated_at"] = "2026-11-07T12:34:56.123456Z";
        QVERIFY(cache.put(cache.lease(), Streams, {row})); cache.close(); QVERIFY(cache.open(Server, Project, User));
        const auto saved = cache.get(cache.lease(), Streams); QVERIFY(saved); QVERIFY(!saved->fresh);
        QCOMPARE(saved->rows.first().toObject().value("updated_at").toString(), row.value("updated_at").toString());
        row["updated_at"]="malformed"; QVERIFY(!cache.put(cache.lease(),Streams,{row}));
        row.remove("updated_at"); QVERIFY(cache.put(cache.lease(),Streams,{row}));
    }

    void directPrivateMetadataSurvivesColdCacheWithoutChangingLegacyLayout()
    {
        QTemporaryDir directory; NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto direct = stream("Direct"); direct["private"] = true; direct["direct_user_uuid"] = UserB;
        QVERIFY(cache.put(lease, Streams, {direct})); cache.close();
        QVERIFY(cache.open(Server, Project, User)); auto saved = cache.get(cache.lease(), Streams);
        QVERIFY(saved); QVERIFY(!saved->fresh); QVERIFY(saved->rows.first().toObject().value("private").toBool());
        QCOMPARE(saved->rows.first().toObject().value("direct_user_uuid").toString(), UserB);
        auto legacy = stream("Legacy"); QVERIFY(cache.put(cache.lease(), Streams, {legacy}));
        saved = cache.get(cache.lease(), Streams); QVERIFY(saved);
        QVERIFY(!saved->rows.first().toObject().contains("private"));
        direct["private"] = "true"; QVERIFY(!cache.put(cache.lease(), Streams, {direct}));
    }

    void budgetReductionDoesNotAccumulateALargeFreelistBeforeReclaiming()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease(); const auto row = message(QString(256 * 1024, QLatin1Char('x')));
        for (int i = 0; i < 40; ++i)
            QVERIFY(cache.put(lease, {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, {row}));
        QString connection;
        for (const auto &name : QSqlDatabase::connectionNames()) {
            const auto database = QSqlDatabase::database(name);
            if (database.databaseName() == directory.filePath("cache.sqlite3")) connection = name;
        }
        QVERIFY(!connection.isEmpty()); bool measured = true; qint64 peakFreeBytes = 0;
        cache.setCancellationCheck([&] {
            QSqlQuery query(QSqlDatabase::database(connection));
            if (!query.exec("PRAGMA freelist_count") || !query.next()) measured = false;
            else peakFreeBytes = std::max(peakFreeBytes, query.value(0).toLongLong() * 4096);
            return false;
        });
        QVERIFY(cache.setDiskLimitBytes(2 * 1024 * 1024)); cache.setCancellationCheck({});
        QVERIFY(measured); qInfo() << "Peak free bytes during LRU reduction:" << peakFreeBytes;
        { QSqlQuery mode(QSqlDatabase::database(connection));
          QVERIFY(mode.exec("PRAGMA auto_vacuum") && mode.next()); QCOMPARE(mode.value(0).toInt(), 2); }
        QVERIFY2(peakFreeBytes <= 2 * 1024 * 1024, qPrintable(QString("Accumulated free bytes: %1").arg(peakFreeBytes)));
        QVERIFY(cache.diskUsageBytes() <= 2 * 1024 * 1024);
        const auto latest = cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "39"});
        QVERIFY(latest); QCOMPARE(latest->rows, QJsonArray{row});
    }

    void compactingEvictionCanCancelAfterADeletionAndResumeWithTheNewestPage()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto row = message(QString(256 * 1024, QLatin1Char('x')));
        for (int i = 0; i < 40; ++i)
            QVERIFY(cache.put(cache.lease(), {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, {row}));
        QString connection;
        for (const auto &name : QSqlDatabase::connectionNames()) {
            const auto database = QSqlDatabase::database(name);
            if (database.databaseName() == directory.filePath("cache.sqlite3")) connection = name;
        }
        QVERIFY(!connection.isEmpty()); bool cancelledAfterDelete = false;
        cache.setCancellationCheck([&] {
            QSqlQuery query(QSqlDatabase::database(connection));
            if (!query.exec("SELECT count(*) FROM entries") || !query.next() || query.value(0).toInt() == 40) return false;
            if (!query.exec("PRAGMA auto_vacuum") || !query.next() || query.value(0).toInt() != 1) return false;
            cancelledAfterDelete = true; return true;
        });
        QVERIFY(!cache.setDiskLimitBytes(2 * 1024 * 1024)); cache.setCancellationCheck({});
        QVERIFY(cancelledAfterDelete); QVERIFY(!cache.available());
        NativeCache reopened(directory.path()); QVERIFY(reopened.open(Server, Project, User));
        const auto latest = reopened.get(reopened.lease(), {NativeCache::Kind::History, Stream, Topic, "39"});
        QVERIFY(latest && !latest->fresh); QCOMPARE(latest->rows, QJsonArray{row});
        QVERIFY(reopened.setDiskLimitBytes(2 * 1024 * 1024)); QVERIFY(reopened.diskUsageBytes() <= 2 * 1024 * 1024);
        QCOMPARE(reopened.get(reopened.lease(), {NativeCache::Kind::History, Stream, Topic, "39"})->rows, QJsonArray{row});
    }

    void init() { QTest::failOnWarning(); }

    void missingHistoryLookupsDoNotReadUnrelatedLargeMessageBodies()
    {
#ifdef Q_OS_LINUX
        const auto readCharacters = []() -> qint64 {
            QFile accounting("/proc/self/io");
            if (!accounting.open(QIODevice::ReadOnly)) return -1;
            for (const auto &line : accounting.readAll().split('\n'))
                if (line.startsWith("rchar:")) return line.mid(6).trimmed().toLongLong();
            return -1;
        };
        QTemporaryDir directory; NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QJsonArray page;
        for (int i = 0; i < 50; ++i) page.append(message(QString(20000, 'x')));
        for (int i = 0; i < 20; ++i) {
            auto key = History; key.marker = QByteArray::number(i);
            QVERIFY(cache.put(lease, key, page));
        }
        const auto before = readCharacters(); QVERIFY(before >= 0);
        for (int i = 0; i < 64; ++i) {
            auto missing = History; missing.marker = "not-written-" + QByteArray::number(i);
            QVERIFY(!cache.get(lease, missing));
        }
        const auto after = readCharacters(); QVERIFY(after >= before);
        const auto read = after - before;
        qInfo("Missing-key lookup read characters: %lld", static_cast<long long>(read));
        QVERIFY2(read < 8 * 1024 * 1024,
            qPrintable(QString("Negative lookups read %1 bytes of unrelated stored bodies").arg(read)));
#else
        QSKIP("Linux per-process I/O accounting regression; other platforms require separate resource acceptance.");
#endif
    }

    void legacyWithoutRowidCacheRemainsReadableWritableAndPhysicallyBounded()
    {
        QTemporaryDir directory; QVERIFY(legacyWithoutRowidCache(directory.path()));
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        QJsonArray page; for (int i = 0; i < 50; ++i) page.append(message(QString(20000, 'x')));
        for (int i = 0; i < 8; ++i) {
            auto key = History; key.marker = QByteArray::number(i); QVERIFY(cache.put(cache.lease(), key, page));
        }
        auto latest = History; latest.marker = "7";
        QVERIFY(cache.get(cache.lease(), latest)); cache.close();
        QVERIFY(cache.open(Server, Project, User));
        const auto restored = cache.get(cache.lease(), latest); QVERIFY(restored); QCOMPARE(restored->rows, page);
        QVERIFY(!restored->fresh);
        QVERIFY(cache.setDiskLimitBytes(4 * 1024 * 1024));
        QVERIFY(cache.diskUsageBytes() <= 4 * 1024 * 1024);
        const auto retained = cache.get(cache.lease(), latest); QVERIFY(retained); QCOMPARE(retained->rows, page);
        cache.close(); QVERIFY(cache.open(Server, Project, User));
        const auto reopened = cache.get(cache.lease(), latest); QVERIFY(reopened); QCOMPARE(reopened->rows, page);
    }


    void topicOrderingMetadataSurvivesColdCacheWithoutReconstruction()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto channel = stream(); channel["notification_mode"] = "muted";
        const QJsonObject topic{{"uuid", Topic}, {"project_id", Project}, {"stream_uuid", Stream}, {"name", "Topic"},
            {"notification_mode", "unmute"}, {"updated_at", "2026-11-07T12:34:56.123456Z"}};
        const NativeCache::Key topics{NativeCache::Kind::Topics, Stream, {}, {}};
        QVERIFY(cache.put(cache.lease(), Streams, {channel})); QVERIFY(cache.put(cache.lease(), topics, {topic}));
        cache.close(); QVERIFY(cache.open(Server, Project, User));
        const auto savedStream = cache.get(cache.lease(), Streams), savedTopic = cache.get(cache.lease(), topics);
        QVERIFY(savedStream && savedTopic); QVERIFY(!savedStream->fresh && !savedTopic->fresh);
        QCOMPARE(savedStream->rows, QJsonArray{channel}); QCOMPARE(savedTopic->rows, QJsonArray{topic});
    }

    void aggregateHistoryKeepsActualTopicsAndFansOutConfirmedChanges()
    {
        QTemporaryDir directory; qint64 now = 10000;
        NativeCache cache(directory.path(), {}, false, [&] { return now; });
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        const NativeCache::Key aggregate{NativeCache::Kind::History, Stream, {}, {}};
        const NativeCache::Key aggregateOlder{NativeCache::Kind::History, Stream, {}, "older"};
        const NativeCache::Key other{NativeCache::Kind::History, Stream, UserB, {}};
        auto first = message("Topic A raw payload");
        first["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", UserB}, {"content", "ciphertext-exact"}};
        first["reactions"] = QJsonObject{}; first["reaction_users"] = QJsonObject{};
        auto second = message("Topic B"); second["topic_uuid"] = UserB;
        QVERIFY(cache.put(lease, History, {first}, "topic-next"));
        QVERIFY(cache.put(lease, other, {second}, "other-next"));
        QVERIFY(cache.put(lease, aggregate, {second, first}, "aggregate-next"));
        QVERIFY(cache.put(lease, aggregateOlder, {first}, "aggregate-older-next"));
        auto invalid = first; invalid["stream_uuid"] = UserB;
        QVERIFY(!cache.put(lease, aggregate, {invalid}));
        invalid = first; invalid.remove("topic_uuid"); QVERIFY(!cache.put(lease, aggregate, {invalid}));
        now = 20000;
        QVERIFY(cache.markHistoryRead(lease, Stream, Topic, {first.value("uuid").toString()}));
        first["read"] = true;
        auto canonical = first; canonical["starred"] = true;
        canonical["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "must not overwrite ciphertext"}};
        QVERIFY(cache.projectMessage(lease, canonical, NativeCache::Projection::Star)); first["starred"] = true;
        canonical["reactions"] = QJsonObject{{"heart", 1}};
        canonical["reaction_users"] = QJsonObject{{"heart", QJsonArray{User}}};
        QVERIFY(cache.projectMessage(lease, canonical, NativeCache::Projection::Reactions));
        first["reactions"] = canonical["reactions"]; first["reaction_users"] = canonical["reaction_users"];
        for (const auto &key : {History, aggregate, aggregateOlder}) {
            const auto saved = cache.get(lease, key); QVERIFY(saved && !saved->fresh);
            QCOMPARE(saved->savedAt, 10000);
            const auto rows = saved->rows; QCOMPARE(rows.last().toObject(), first);
        }
        const auto otherSaved = cache.get(lease, other); QVERIFY(otherSaved && otherSaved->fresh);
        QCOMPARE(otherSaved->rows, QJsonArray{second});
        QCOMPARE(cache.get(lease, aggregate)->nextMarker, QByteArray("aggregate-next"));
        cache.close(); QVERIFY(cache.open(Server, Project, User));
        const auto cold = cache.get(cache.lease(), aggregate); QVERIFY(cold && !cold->fresh);
        QCOMPARE(cold->rows, QJsonArray({second, first}));
        QVERIFY(cache.invalidate(cache.lease(), NativeCache::Kind::History, Stream, Topic));
        QVERIFY(!cache.get(cache.lease(), History)); QVERIFY(!cache.get(cache.lease(), aggregate));
        QVERIFY(!cache.get(cache.lease(), aggregateOlder)); QVERIFY(cache.get(cache.lease(), other));
        QVERIFY(cache.projectMessage(cache.lease(), first, NativeCache::Projection::Insert));
        QVERIFY(!cache.get(cache.lease(), aggregate));
    }

    void aggregateSchemaMigrationPreservesVersionTwoTopicPages()
    {
        QTemporaryDir directory; NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, User)); const auto row = message();
        QVERIFY(cache.put(cache.lease(), History, {row}, "retained")); cache.close();
        QVERIFY(modify(directory.path(), "PRAGMA user_version=2"));
        QVERIFY(cache.open(Server, Project, User)); const auto saved = cache.get(cache.lease(), History);
        QVERIFY(saved && !saved->fresh); QCOMPARE(saved->rows, QJsonArray{row});
        QCOMPARE(saved->nextMarker, QByteArray("retained")); cache.close();
        const auto name = QUuid::createUuid().toString();
        int version = -1;
        { auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(directory.filePath("cache.sqlite3"));
          QVERIFY(db.open()); { QSqlQuery query(db); QVERIFY(query.exec("PRAGMA user_version")); QVERIFY(query.next()); version = query.value(0).toInt(); } db.close(); }
        QSqlDatabase::removeDatabase(name); QCOMPARE(version, 4);
    }

    void canonicalProjectionChangesOnlyItsFieldsAndPreservesRawPages_data()
    {
        QTest::addColumn<int>("fields");
        QTest::addColumn<bool>("aggregate");
        QTest::newRow("payload") << int(NativeCache::Projection::Payload) << false;
        QTest::newRow("star") << int(NativeCache::Projection::Star) << false;
        QTest::newRow("reactions") << int(NativeCache::Projection::Reactions) << false;
        QTest::newRow("aggregate-payload") << int(NativeCache::Projection::Payload) << true;
        QTest::newRow("aggregate-star") << int(NativeCache::Projection::Star) << true;
        QTest::newRow("aggregate-reactions") << int(NativeCache::Projection::Reactions) << true;
    }
    void canonicalProjectionChangesOnlyItsFieldsAndPreservesRawPages()
    {
        QFETCH(int, fields); QFETCH(bool, aggregate);
        const NativeCache::Key primary{NativeCache::Kind::History, Stream, aggregate ? QString{} : Topic, {}};
        QTemporaryDir directory; qint64 now = 10000;
        NativeCache cache(directory.path(), {}, false, [&] { return now; });
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto original = message("Original payload"); original["read"] = true;
        original["reactions"] = QJsonObject{}; original["reaction_users"] = QJsonObject{};
        if (fields != int(NativeCache::Projection::Payload))
            original["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", UserB}, {"content", "raw-ciphertext-retained-exactly"}};
        auto untouched = message("Other message");
        const NativeCache::Key older{NativeCache::Kind::History, Stream, primary.topic, "older"};
        const NativeCache::Key other{NativeCache::Kind::History, Stream, UserB, {}};
        auto otherRow = original; otherRow["topic_uuid"] = UserB;
        // A full canonical payload now owns placement. Its isolation control is
        // a different identity; partial star/reaction responses still prove that
        // their same-UUID copy in another route is untouched.
        if (fields == int(NativeCache::Projection::Payload)) otherRow["uuid"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(cache.put(lease, primary, {original, untouched}, "next-head"));
        QVERIFY(cache.put(lease, older, {original}, "next-older"));
        QVERIFY(cache.put(lease, other, {otherRow}));
        auto canonical = original;
        canonical["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "New canonical payload"}};
        canonical["starred"] = true; canonical["read"] = false;
        canonical["reactions"] = QJsonObject{{"heart", 2}};
        canonical["reaction_users"] = QJsonObject{{"heart", QJsonArray{User, UserB}}};
        now = 20000; const auto scans = cache.stats().fullScans;
        QVERIFY(cache.projectMessage(lease, canonical, NativeCache::ProjectionFields::fromInt(fields)));
        auto expected = original;
        if (fields == int(NativeCache::Projection::Payload)) expected["payload"] = canonical["payload"];
        if (fields == int(NativeCache::Projection::Star)) expected["starred"] = true;
        if (fields == int(NativeCache::Projection::Reactions)) {
            expected["reactions"] = canonical["reactions"]; expected["reaction_users"] = canonical["reaction_users"];
        }
        const auto head = cache.get(lease, primary), old = cache.get(lease, older), unaffected = cache.get(lease, other);
        QVERIFY(head && old && unaffected);
        QCOMPARE(head->rows, QJsonArray({expected, untouched})); QCOMPARE(old->rows, QJsonArray{expected});
        QVERIFY(!head->fresh && !old->fresh); QVERIFY(unaffected->fresh);
        QCOMPARE(head->savedAt, 10000); QCOMPARE(old->savedAt, 10000);
        QCOMPARE(head->nextMarker, QByteArray("next-head")); QCOMPARE(old->nextMarker, QByteArray("next-older"));
        QCOMPARE(unaffected->rows, QJsonArray{otherRow}); QCOMPARE(cache.stats().fullScans, scans);
    }

    void confirmedInsertUsesOnlyAnExistingBoundedHeadAndNeverReplacesARow()
    {
        QTemporaryDir directory; qint64 now = 10000;
        NativeCache cache(directory.path(), {}, false, [&] { return now; });
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QJsonArray head;
        for (int i = 0; i < 50; ++i) {
            auto row = message(QString::number(i));
            row["created_at"] = QString("2026-11-06T12:34:56.%1Z").arg(100 + i, 6, 10, QLatin1Char('0'));
            head.prepend(row);
        }
        const NativeCache::Key older{NativeCache::Kind::History, Stream, Topic, "old-boundary"};
        auto old = message("Older raw page"); old["created_at"] = "2026-11-06T12:34:56.000050Z";
        QVERIFY(cache.put(lease, History, head, "old-boundary")); QVERIFY(cache.put(lease, older, {old}, "next-old"));
        auto sent = message("Confirmed send"); sent["created_at"] = "2026-11-06T12:34:56.000200Z";
        now = 20000;
        QVERIFY(cache.projectMessage(lease, sent, NativeCache::Projection::Insert));
        const auto saved = cache.get(lease, History); QVERIFY(saved && !saved->fresh);
        QCOMPARE(saved->rows.size(), 50); QCOMPARE(saved->rows.first().toObject(), sent);
        QCOMPARE(saved->rows.last(), head.at(48)); QCOMPARE(saved->savedAt, 10000);
        QCOMPARE(saved->nextMarker, QByteArray("old-boundary"));
        const auto retainedOlder = cache.get(lease, older); QVERIFY(retainedOlder && !retainedOlder->fresh);
        QCOMPARE(retainedOlder->rows, QJsonArray{old}); QCOMPARE(retainedOlder->nextMarker, QByteArray("next-old"));
        auto stale = sent; stale["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Stale duplicate"}};
        QVERIFY(cache.projectMessage(lease, stale, NativeCache::Projection::Insert));
        QCOMPARE(cache.get(lease, History)->rows.first().toObject(), sent);
        QVERIFY(cache.invalidate(lease, NativeCache::Kind::History, Stream, Topic));
        QVERIFY(cache.projectMessage(lease, sent, NativeCache::Projection::Insert));
        QVERIFY(!cache.get(lease, History));
    }

    void oversizedProjectionRetainsOldStaleDataAndCannotUseAForeignLease()
    {
        QTemporaryDir directory; NativeCache::Policy policy;
        policy.maxEntryBytes = 1024; policy.maxBytes = 4096;
        NativeCache cache(directory.path(), policy, false); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease(); const auto row = message("Retained");
        QVERIFY(cache.put(lease, History, {row}, "next"));
        auto canonical = row; canonical["payload"] = QJsonObject{{"kind", "markdown"}, {"content", QString(1024, 'x')}};
        QVERIFY(!cache.projectMessage(lease, canonical, NativeCache::Projection::Payload));
        const auto retained = cache.get(lease, History); QVERIFY(retained && !retained->fresh);
        QCOMPARE(retained->rows, QJsonArray{row}); QCOMPARE(retained->nextMarker, QByteArray("next"));
        QVERIFY(cache.open(Server, Project, UserB));
        QVERIFY(!cache.projectMessage(lease, canonical, NativeCache::Projection::Payload));
        QVERIFY(!cache.get(cache.lease(), History));
    }

    void projectionPreservesEncodedControlPayloadAndIndexesInsertedRows()
    {
        QTemporaryDir directory; NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto row = message("Unused plaintext");
        row["payload"] = QJsonObject{{"kind", "key_grant"}, {"key_uuid", UserB}, {"request_message_uuid", Topic},
            {"sender_device_id", "sender"}, {"recipient_device_id", "recipient"}, {"content", "raw-encoded-grant"}};
        row["read"] = false;
        QVERIFY(cache.put(lease, History, {row}, "cursor"));
        auto canonical = row; canonical["starred"] = true;
        canonical["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Must not replace raw grant"}};
        QVERIFY(cache.projectMessage(lease, canonical, NativeCache::Projection::Star));
        auto saved = cache.get(lease, History); QVERIFY(saved);
        QCOMPARE(saved->rows.first().toObject().value("payload"), row.value("payload"));
        auto inserted = message("New raw row"); inserted["read"] = false;
        inserted["created_at"] = "2026-11-07T12:00:00Z";
        QVERIFY(cache.projectMessage(lease, inserted, NativeCache::Projection::Insert));
        QVERIFY(cache.markHistoryRead(lease, Stream, Topic, {inserted.value("uuid").toString()}));
        saved = cache.get(lease, History); QVERIFY(saved && !saved->fresh);
        QCOMPARE(saved->nextMarker, QByteArray("cursor"));
        QVERIFY(saved->rows.first().toObject().value("read").toBool());
        QCOMPARE(saved->rows.at(1).toObject().value("payload"), row.value("payload"));
        QVERIFY(!saved->rows.at(1).toObject().value("read").toBool());
    }

    void projectionSqlFailureClosesTheOptionalStoreWithoutActiveQueries()
    {
        QTemporaryDir directory; NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        const auto row = message("Validated raw row"); QVERIFY(cache.put(lease, History, {row}));
        QVERIFY(modify(directory.path(), "DROP TABLE history_rows"));
        QSignalSpy errors(&cache, &NativeCache::errorChanged);
        QVERIFY(!cache.projectMessage(lease, row, NativeCache::Projection::Payload));
        QVERIFY(!cache.available()); QCOMPARE(errors.size(), 1);
        QVERIFY(!cache.error().contains("Validated raw row"));
    }

    void scopedFreshnessDoesNotReadUnrelatedDiskPages()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QVERIFY(cache.put(lease, Streams, {stream()})); QVERIFY(cache.put(lease, History, {message()}));
        QVERIFY(modify(directory.path(), "DROP TABLE entries"));
        QSignalSpy errors(&cache, &NativeCache::errorChanged);
        QVERIFY(cache.markStale(lease, NativeCache::Kind::History, Stream, Topic));
        QCOMPARE(errors.size(), 0); // This operation clears bounded RAM freshness only.
        cache.close();
    }

    void existingSchemaWithoutIncrementalVacuumIsRejectedBeforeMigration_data()
    {
        QTest::addColumn<int>("schemaVersion");
        QTest::newRow("v1") << 1; QTest::newRow("v2") << 2;
    }
    void existingSchemaWithoutIncrementalVacuumIsRejectedBeforeMigration()
    {
        QFETCH(int, schemaVersion); QTemporaryDir directory;
        {
            NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
            for (int i = 0; i < 32; ++i)
                QVERIFY(cache.put(cache.lease(), {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, {message(QString(128 * 1024, 'x'))}));
        }
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection); database.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(database.open());
            { QSqlQuery query(database); if (schemaVersion == 1) QVERIFY(query.exec("DROP TABLE history_rows"));
              QVERIFY(query.exec("PRAGMA user_version=" + QString::number(schemaVersion)));
              QVERIFY(query.exec("PRAGMA auto_vacuum=NONE")); QVERIFY(query.exec("VACUUM"));
              QVERIFY(query.exec("PRAGMA auto_vacuum")); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(), 0); }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        QFile before(directory.filePath("cache.sqlite3")); QVERIFY(before.open(QIODevice::ReadOnly)); const auto bytes = before.readAll(); before.close();
        NativeCache cache(directory.path());
        QVERIFY(!cache.open(Server, Project, User)); QVERIFY(!cache.available());
        QVERIFY(before.open(QIODevice::ReadOnly)); QCOMPARE(before.readAll(), bytes);
    }

    void fullAutoVacuumCanSwitchToIncrementalWithoutLosingRows()
    {
        QTemporaryDir directory; auto row = message("supported SQLite FULL pointer-map layout");
        { NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User)); QVERIFY(cache.put(cache.lease(), History, {row}, "next")); }
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection); database.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(database.open());
            { QSqlQuery query(database); QVERIFY(query.exec("PRAGMA auto_vacuum=FULL"));
              QVERIFY(query.exec("PRAGMA auto_vacuum")); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(), 1); }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto saved = cache.get(cache.lease(), History); QVERIFY(saved && !saved->fresh);
        QCOMPARE(saved->rows, QJsonArray{row}); QCOMPARE(saved->nextMarker, QByteArray("next"));
        QVERIFY(cache.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
        QVERIFY(cache.diskUsageBytes() <= cache.diskLimitBytes());
    }

    void configurablePhysicalBudgetDefaultsToDecimalTenGigabytesWithoutPreallocation()
    {
        QTemporaryDir directory; NativeCache cache(directory.path());
        QCOMPARE(cache.diskLimitBytes(), 10000000000LL); QCOMPARE(cache.diskUsageBytes(), 0);
        QSignalSpy errors(&cache, &NativeCache::errorChanged);
        QVERIFY(cache.setDiskLimitBytes(20000000000LL)); QCOMPARE(cache.diskLimitBytes(), 20000000000LL);
        for (const qint64 invalid : {qint64(0), NativeCache::MinimumDiskLimitBytes - 1, NativeCache::MaximumDiskLimitBytes + 1}) {
            QVERIFY(!cache.setDiskLimitBytes(invalid)); QCOMPARE(cache.diskLimitBytes(), 20000000000LL);
            QVERIFY(!cache.error().isEmpty());
        }
        QVERIFY(errors.size() >= 1);
        QVERIFY(cache.setDiskLimitBytes(NativeCache::DefaultDiskLimitBytes)); QVERIFY(cache.error().isEmpty());
        QVERIFY(cache.open(Server, Project, User)); QVERIFY(cache.diskUsageBytes() > 0);
        QVERIFY(cache.diskUsageBytes() < 1024 * 1024); // No10GB eager allocation.
    }

    void defaultRetentionExceedsTheOldPageAndDatabaseCeilings()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease(); const auto row = message(QString(128 * 1024, QLatin1Char('x')));
        for (int i = 0; i < 600; ++i) {
            auto key = History; key.marker = QByteArray::number(i); QVERIFY(cache.put(lease, key, {row}));
        }
        QCOMPARE(cache.stats().entries, 600);
        QVERIFY(cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "0"}));
        QVERIFY(cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "599"}));
        QVERIFY(cache.diskUsageBytes() > 64 * 1024 * 1024); QVERIFY(cache.diskUsageBytes() <= cache.diskLimitBytes());
    }

    void loweringThePhysicalBudgetEvictsLruAndReclaimsDatabaseAndJournal()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease(); const auto row = message(QString(128 * 1024, QLatin1Char('x')));
        for (int i = 0; i < 100; ++i) {
            auto key = History; key.marker = QByteArray::number(i); QVERIFY(cache.put(lease, key, {row}));
        }
        QCOMPARE(cache.stats().entries, 100); QVERIFY(cache.diskUsageBytes() > 8 * 1024 * 1024);
        constexpr qint64 reduced = 4 * 1024 * 1024;
        const auto checkpoints = cache.stats().checkpoints;
        QVERIFY(cache.setDiskLimitBytes(reduced)); QCOMPARE(cache.lease().generation, lease.generation);
        QVERIFY(cache.available()); QVERIFY(cache.diskUsageBytes() <= reduced); QVERIFY(cache.stats().entries < 100);
        // Reclaiming several MiB must not checkpoint after each 4KiB page. This
        // resource bound is independent of filesystem speed and CPU scheduling.
        QVERIFY2(cache.stats().checkpoints - checkpoints < 64,
            qPrintable(QString("Resize checkpoints: %1").arg(cache.stats().checkpoints - checkpoints)));
        qInfo() << "Physical resize checkpoints:" << cache.stats().checkpoints - checkpoints;
        QVERIFY(!cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "0"}));
        QVERIFY(cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "99"}));
        cache.close(); NativeCache reopened(directory.path()); QVERIFY(reopened.setDiskLimitBytes(reduced));
        QVERIFY(reopened.open(Server, Project, User)); QVERIFY(reopened.diskUsageBytes() <= reduced);
        const auto saved = reopened.get(reopened.lease(), {NativeCache::Kind::History, Stream, Topic, "99"});
        QVERIFY(saved && !saved->fresh);
    }

    void cancelledVacuumTransactionLeavesAReopenableDatabaseAndPreservesTheNewestPage()
    {
        QTemporaryDir directory;
        const auto newest = message(QString(128 * 1024, QLatin1Char('x')));
        const NativeCache::Key newestKey{NativeCache::Kind::History, Stream, Topic, "99"};
        {
            NativeCache seed(directory.path()); QVERIFY(seed.open(Server, Project, User));
            for (int i = 0; i < 100; ++i)
                QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, {newest}));
        }
        // Leave a large freelist with no eviction needed, so cancellation lands
        // during physical reclamation rather than an unrelated LRU DELETE.
        QVERIFY(modify(directory.path(), "DELETE FROM entries WHERE marker!=CAST('99' AS BLOB)", true));
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        QVERIFY(cache.diskUsageBytes() > 8 * 1024 * 1024);
        int checks = 0;
        cache.setCancellationCheck([&checks] { return ++checks >= 32; });
        QVERIFY(!cache.setDiskLimitBytes(4 * 1024 * 1024));
        QVERIFY(!cache.available()); QVERIFY(checks >= 32);
        cache.setCancellationCheck({});
        NativeCache reopened(directory.path()); QVERIFY(reopened.setDiskLimitBytes(4 * 1024 * 1024));
        QVERIFY(reopened.open(Server, Project, User));
        QVERIFY(reopened.diskUsageBytes() <= reopened.diskLimitBytes());
        const auto saved = reopened.get(reopened.lease(), newestKey);
        QVERIFY(saved && !saved->fresh); QCOMPARE(saved->rows, QJsonArray{newest});
    }

    void warmOperationsWithThousandsOfPagesAvoidWholeCacheScansAndYieldToTheEventLoop()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease(); const auto row = message(QString(1024, QLatin1Char('x')));
        const auto scans = cache.stats().fullScans; QVector<qint64> samples;
        QElapsedTimer total; total.start();
        for (int i = 0; i < NativeCache::MaximumFreshPages + 256; ++i) {
            auto key = History; key.marker = QByteArray::number(i); QElapsedTimer step; step.start();
            QVERIFY(cache.put(lease, key, {row}));
            if (i >= NativeCache::MaximumFreshPages) samples.append(step.nsecsElapsed() / 1000);
        }
        QCOMPARE(cache.stats().entries, NativeCache::MaximumFreshPages + 256);
        QVERIFY(cache.stats().freshPages <= NativeCache::MaximumFreshPages); QCOMPARE(cache.stats().fullScans, scans);
        bool advanced = false; qint64 dispatchMicros = -1;
        QTimer::singleShot(0, &cache, [&] {
            QElapsedTimer step; step.start();
            auto key = History; key.marker = "event-loop";
            const bool okay = cache.put(lease, key, {row}) && cache.get(lease, key).has_value();
            dispatchMicros = step.nsecsElapsed() / 1000;
            QTimer::singleShot(0, &cache, [&, okay] { advanced = okay; });
        });
        QTRY_VERIFY(advanced); QCOMPARE(cache.stats().fullScans, scans);
        std::sort(samples.begin(), samples.end());
        qInfo().nospace() << "4352 pages: fill=" << total.elapsed() << "ms, last256 put p50=" << samples[samples.size()/2]
            << "us p95=" << samples[samples.size()*95/100] << "us, event-loop put/get=" << dispatchMicros
            << "us, full scans during warm operations=0, fresh RAM pages=" << cache.stats().freshPages;
    }

    void indexedReadPatchDoesNotParseUnrelatedSavedPayloads()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto first = message("captured viewport"); auto other = message("unrelated saved page");
        QVERIFY(cache.put(lease, History, {first}));
        const NativeCache::Key unrelated{NativeCache::Kind::History, Stream, Topic, "unrelated"};
        QVERIFY(cache.put(lease, unrelated, {other}));
        QVERIFY(modify(directory.path(), "UPDATE entries SET payload='invalid unrelated JSON' WHERE marker=CAST('unrelated' AS BLOB)"));
        QVERIFY(cache.markHistoryRead(lease, Stream, Topic, {first.value("uuid").toString()}));
        const auto patched = cache.get(lease, History); QVERIFY(patched && !patched->fresh);
        QVERIFY(patched->rows.first().toObject().value("read").toBool());
        QVERIFY(!cache.get(lease, unrelated));
    }

    void schemaOneMigrationRebuildsTheMessagePageIndexWithoutLosingRawRows()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = message("previous native cache"); QVERIFY(cache.put(cache.lease(), History, {row}, "next")); cache.close();
        QVERIFY(modify(directory.path(), "DROP TABLE history_rows")); QVERIFY(modify(directory.path(), "PRAGMA user_version=1"));
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QVERIFY(cache.markHistoryRead(lease, Stream, Topic, {row.value("uuid").toString()}));
        row["read"] = true; const auto saved = cache.get(lease, History); QVERIFY(saved && !saved->fresh);
        QCOMPARE(saved->rows, QJsonArray{row}); QCOMPARE(saved->nextMarker, QByteArray("next"));
    }

    void externalWritesInvalidateWarmTrustAndRecomputeActualBytesOnce()
    {
        QTemporaryDir directory; NativeCache::Policy policy; policy.maxBytes = 4096; policy.maxEntryBytes = 4096;
        NativeCache cache(directory.path(), policy, false); QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QVERIFY(cache.put(lease, History, {message(QString(2400, QLatin1Char('x')))})); const auto scans = cache.stats().fullScans;
        QVERIFY(modify(directory.path(), "UPDATE entries SET size=1"));
        const auto saved = cache.get(lease, History); QVERIFY(saved && !saved->fresh);
        QCOMPARE(cache.stats().fullScans, scans + 1); QVERIFY(cache.stats().bytes > 2400);
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, Stream, Topic, "new"}, {message(QString(2400, QLatin1Char('y')))}));
        QCOMPARE(cache.stats().entries, 1); QVERIFY(cache.stats().bytes <= policy.maxBytes);
        QCOMPARE(cache.stats().fullScans, scans + 1);
    }

    void laterIndexFailureIsOptionalAndReportsItsSafeErrorWithoutWarnings()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease(); const auto row = message(); QVERIFY(cache.put(lease, History, {row}));
        QSignalSpy errors(&cache, &NativeCache::errorChanged); QVERIFY(modify(directory.path(), "DROP TABLE history_rows"));
        QVERIFY(!cache.markHistoryRead(lease, Stream, Topic, {row.value("uuid").toString()}));
        QVERIFY(!cache.available()); QVERIFY(!cache.error().isEmpty()); QCOMPARE(errors.size(), 1);
    }

    void minimumPhysicalBudgetIncludesJournalAndCanGrowWithoutChangingOwnerLease()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); constexpr auto minimum = NativeCache::MinimumDiskLimitBytes;
        QVERIFY(cache.setDiskLimitBytes(minimum)); QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        const auto row = message(QString(32 * 1024, QLatin1Char('x')));
        for (int i = 0; i < 300; ++i) {
            auto key = History; key.marker = QByteArray::number(i); QVERIFY(cache.put(lease, key, {row}));
            QVERIFY(cache.diskUsageBytes() <= minimum);
        }
        QVERIFY(cache.stats().entries < 300); QVERIFY(cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "299"}));
        QVERIFY(cache.stats().sampledPeakDiskBytes <= minimum);
        qInfo().nospace() << "1MiB physical budget: final=" << cache.diskUsageBytes()
            << " bytes, sampled post-commit DB+WAL+SHM peak=" << cache.stats().sampledPeakDiskBytes << " bytes";
        QVERIFY(cache.setDiskLimitBytes(4 * minimum)); QCOMPARE(cache.lease().generation, lease.generation);
        for (int i = 300; i < 400; ++i) {
            auto key = History; key.marker = QByteArray::number(i); QVERIFY(cache.put(lease, key, {row}));
        }
        QVERIFY(cache.diskUsageBytes() > minimum); QVERIFY(cache.diskUsageBytes() <= 4 * minimum);
    }

    void openingAnExistingLargerCacheReclaimsItBeforePublishingTheOwner()
    {
        QTemporaryDir directory; const auto row = message(QString(128 * 1024, QLatin1Char('x')));
        {
            NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
            for (int i = 0; i < 60; ++i) QVERIFY(cache.put(cache.lease(), {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, {row}));
            QVERIFY(cache.diskUsageBytes() > 2 * 1024 * 1024);
        }
        NativeCache cache(directory.path()); QVERIFY(cache.setDiskLimitBytes(2 * 1024 * 1024));
        QVERIFY(cache.diskUsageBytes() > cache.diskLimitBytes());
        QElapsedTimer elapsed; elapsed.start(); QVERIFY(cache.open(Server, Project, User));
        QVERIFY(cache.diskUsageBytes() <= cache.diskLimitBytes());
        QVERIFY(cache.get(cache.lease(), {NativeCache::Kind::History, Stream, Topic, "59"}));
        qInfo().nospace() << "8MiB cold cache shrink to2MiB: " << elapsed.elapsed() << "ms, final=" << cache.diskUsageBytes() << " bytes";
    }

    void hotReadLruBookkeepingCannotGrowTheWalBeyondThePhysicalLimit()
    {
        QTemporaryDir directory; NativeCache cache(directory.path()); QVERIFY(cache.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        for (int i = 0; i < 15; ++i) QVERIFY(cache.put(lease, {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, {message(QString(32 * 1024, QLatin1Char('x')))}));
        for (int i = 0; i < 200; ++i) {
            QVERIFY(cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "14"}));
            QVERIFY2(cache.diskUsageBytes() <= cache.diskLimitBytes(), "LRU read writes must share the DB+WAL+SHM budget");
        }
    }

    void scopedStaleAndConfirmedReadKeepRawPayloadCursorAgeAndOtherOwners()
    {
        QTemporaryDir directory; qint64 now = 10000;
        NativeCache cache(directory.path(), {}, false, [&] { return now; });
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto encrypted = message(); encrypted["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", UserB}, {"content", "opaque-byte-equivalent"}};
        const auto id = encrypted.value("uuid").toString();
        auto untouched = message("not captured by the confirmed viewport");
        auto otherRoute = untouched; otherRoute["topic_uuid"] = UserB;
        const NativeCache::Key older{NativeCache::Kind::History, Stream, Topic, "older-page"};
        const NativeCache::Key other{NativeCache::Kind::History, Stream, UserB, {}};
        QVERIFY(cache.put(lease, Streams, {stream()}));
        QVERIFY(cache.put(lease, History, {encrypted, untouched}, "next-page"));
        QVERIFY(cache.put(lease, older, {encrypted}, "next-older-page"));
        QVERIFY(cache.put(lease, other, {otherRoute}));
        now += 100;
        QVERIFY(cache.markStale(lease, NativeCache::Kind::History, Stream, Topic));
        QVERIFY(!cache.get(lease, History)->fresh); QVERIFY(!cache.get(lease, older)->fresh);
        QVERIFY(cache.get(lease, other)->fresh); QVERIFY(cache.get(lease, Streams)->fresh);
        QVERIFY(cache.markHistoryRead(lease, Stream, Topic, {id}));
        const auto patched = cache.get(lease, History); QVERIFY(patched && !patched->fresh);
        auto expected = encrypted; expected["read"] = true;
        QCOMPARE(patched->rows, QJsonArray({expected, untouched}));
        QCOMPARE(patched->nextMarker, QByteArray("next-page")); QCOMPARE(patched->savedAt, 10000);
        QCOMPARE(cache.get(lease, older)->rows, QJsonArray{expected});
        QCOMPARE(cache.get(lease, older)->nextMarker, QByteArray("next-older-page"));
        QVERIFY(!cache.get(lease, other)->rows.first().toObject().value("read").toBool());
        QVERIFY(!cache.markHistoryRead(lease, Stream, Topic, {"malformed"}));
        QVERIFY(cache.open(Server, Project, UserB)); const auto foreign = cache.lease();
        QVERIFY(cache.put(foreign, History, {encrypted}));
        QVERIFY(!cache.markHistoryRead(lease, Stream, Topic, {id}));
        QVERIFY(!cache.markStale(lease, NativeCache::Kind::History, Stream, Topic));
        QVERIFY(!cache.get(foreign, History)->rows.first().toObject().value("read").toBool());
        QVERIFY(cache.open(Server, Project, User)); const auto restored = cache.get(cache.lease(), History);
        QVERIFY(restored && !restored->fresh); QCOMPARE(restored->rows, QJsonArray({expected, untouched}));
    }

    void confirmedReadCannotGrowAStoredPageBeyondItsByteLimit()
    {
        QTemporaryDir directory; NativeCache::Policy policy; policy.maxBytes = 2048; policy.maxEntryBytes = 1024;
        NativeCache cache(directory.path(), policy, false); QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        auto row = message(""); row.remove("read");
        const auto padding = 1024 - 256 - QJsonDocument(QJsonArray{row}).toJson(QJsonDocument::Compact).size();
        QVERIFY(padding > 0); row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", QString(padding, QLatin1Char('x'))}};
        QVERIFY(cache.put(lease, History, {row}));
        QCOMPARE(cache.stats().bytes, 1024);
        QVERIFY(!cache.markHistoryRead(lease, Stream, Topic, {row.value("uuid").toString()}));
        const auto retained = cache.get(lease, History); QVERIFY(retained && !retained->fresh);
        QCOMPARE(retained->rows, QJsonArray{row}); QVERIFY(cache.stats().bytes <= policy.maxBytes);
    }

    void confirmedReadRetainsTheGlobalByteBudgetAcrossSeveralPages()
    {
        QTemporaryDir directory; NativeCache::Policy policy; policy.maxBytes = 4096; policy.maxEntryBytes = 2048;
        NativeCache cache(directory.path(), policy, false); QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QSet<QString> confirmed;
        for (int i = 0; i < 4; ++i) {
            auto row = message(""); row.remove("read"); confirmed.insert(row.value("uuid").toString());
            const QByteArray marker = QByteArray("p") + QByteArray::number(i);
            const auto padding = 1018 - 256 - marker.size() - QJsonDocument(QJsonArray{row}).toJson(QJsonDocument::Compact).size();
            QVERIFY(padding > 0); row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", QString(padding, QLatin1Char('x'))}};
            QVERIFY(cache.put(lease, {NativeCache::Kind::History, Stream, Topic, marker}, {row}));
        }
        QCOMPARE(cache.stats().entries, 4); QCOMPARE(cache.stats().bytes, 4072);
        QVERIFY(cache.markHistoryRead(lease, Stream, Topic, confirmed));
        QVERIFY(cache.stats().bytes <= policy.maxBytes); QCOMPARE(cache.stats().entries, 3);
        QVERIFY(!cache.get(lease, {NativeCache::Kind::History, Stream, Topic, "p0"}));
        for (const auto *marker : {"p1", "p2", "p3"}) {
            const auto saved = cache.get(lease, {NativeCache::Kind::History, Stream, Topic, marker});
            QVERIFY(saved && !saved->fresh); QVERIFY(saved->rows.first().toObject().value("read").toBool());
        }
    }

    void persistsButRequiresRevalidationAfterRestart()
    {
        QTemporaryDir directory;
        const auto row = message();
        NativeCache::Lease old;
        {
            NativeCache cache(directory.path());
            QVERIFY(cache.open(Server, Project, User)); old = cache.lease();
            QVERIFY(cache.put(old, History, {row}, "next-page"));
            const auto cached = cache.get(old, History);
            QVERIFY(cached && cached->fresh); QCOMPARE(cached->rows.first().toObject(), row);
            QCOMPARE(cached->nextMarker, QByteArray("next-page"));
        }
        NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, User));
        const auto saved = cache.get(cache.lease(), History);
        QVERIFY(saved && !saved->fresh);
        QCOMPARE(saved->rows.first().toObject().value("created_at").toString(), QString("2026-11-06T12:34:56.123456Z"));
        QVERIFY(cache.put(cache.lease(), History, saved->rows));
        QVERIFY(cache.get(cache.lease(), History)->fresh);
        cache.markStale(cache.lease()); QVERIFY(!cache.get(cache.lease(), History)->fresh);
    }

    void canonicalOwnersAndGenerationLeasesAreIsolated()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path());
        QVERIFY(cache.open(QUrl("https://WORKSPACE.EXAMPLE.TEST:443/"), Project.toUpper(), User.toUpper()));
        const auto a = cache.lease(); QVERIFY(cache.put(a, Streams, {stream("Owner A")}));
        QVERIFY(cache.open(Server, Project, UserB));
        const auto b = cache.lease();
        QVERIFY(!cache.get(b, Streams)); QVERIFY(!cache.put(a, Streams, {stream("Late response")}));
        QVERIFY(!cache.clearOwner(a)); QVERIFY(cache.put(b, Streams, {stream("Owner B")}));
        QVERIFY(cache.open(QUrl("https://other.example.test"), Project, User));
        QVERIFY(!cache.get(cache.lease(), Streams));
        QVERIFY(cache.open(Server, UserB, User)); QVERIFY(!cache.get(cache.lease(), Streams));
        QVERIFY(cache.open(Server, Project, User));
        const auto restored = cache.get(cache.lease(), Streams);
        QVERIFY(restored && !restored->fresh);
        QCOMPARE(restored->rows.first().toObject().value("name").toString(), QString("Owner A"));
        QVERIFY(!cache.get(a, Streams));
        QVERIFY(cache.clearOwner(cache.lease())); QVERIFY(!cache.get(cache.lease(), Streams));
        QVERIFY(cache.open(Server, Project, UserB));
        QCOMPARE(cache.get(cache.lease(), Streams)->rows.first().toObject().value("name").toString(), QString("Owner B"));
    }

    void ttlAndClockRollbackCannotMakeRowsAuthoritative()
    {
        QTemporaryDir directory;
        qint64 now = 10000;
        NativeCache::Policy policy; policy.ttlMs = 2000;
        NativeCache cache(directory.path(), policy, false, [&] { return now; });
        QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        QVERIFY(cache.put(lease, Streams, {stream()}));
        now = 11999; QVERIFY(cache.get(lease, Streams)->fresh);
        now = 12000; QVERIFY(!cache.get(lease, Streams)->fresh);
        now = 9999; QVERIFY(!cache.get(lease, Streams)->fresh);
    }

    void forgettingAnInactiveOrSignedOutAccountDeletesOnlyItsPartition()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path());
        QVERIFY(cache.open(Server, Project, UserB)); QVERIFY(cache.put(cache.lease(), Streams, {stream("B")}));
        QVERIFY(cache.open(Server, Project, User)); auto current = cache.lease();
        QVERIFY(cache.put(current, Streams, {stream("A")}));
        QVERIFY(cache.forgetAccount(Server, Project, UserB));
        QCOMPARE(cache.lease().owner, current.owner); QCOMPARE(cache.lease().generation, current.generation);
        QVERIFY(cache.get(current, Streams)->fresh); QCOMPARE(cache.stats().entries, 1);
        QVERIFY(cache.open(Server, Project, UserB)); QVERIFY(!cache.get(cache.lease(), Streams));
        cache.close(); QVERIFY(cache.forgetAccount(Server, Project, User)); QVERIFY(!cache.available());
        QVERIFY(cache.open(Server, Project, User)); QVERIFY(!cache.get(cache.lease(), Streams));
    }

    void aLeaseFromAReplacedServiceCannotWriteIntoTheReplacement()
    {
        QTemporaryDir directory;
        NativeCache::Lease previous;
        {
            NativeCache original(directory.path()); QVERIFY(original.open(Server, Project, User));
            previous = original.lease();
        }
        NativeCache replacement(directory.path()); QVERIFY(replacement.open(Server, Project, User));
        QVERIFY(!replacement.put(previous, Streams, {stream("Late old-service response")}));
        QVERIFY(!replacement.get(replacement.lease(), Streams));
    }

    void typedWhitelistExcludesCredentialsAndPreservesEncodedPayload()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = message();
        row.insert("access_token", "never-cache-this-fixture-token");
        auto payload = QJsonObject{{"kind", "e2ee"}, {"key_uuid", Topic}, {"content", "encoded-ciphertext-fixture"},
            {"private_key", "never-cache-this-fixture-key"}};
        row.insert("payload", payload);
        QVERIFY(cache.put(cache.lease(), History, {row}));
        const auto result = cache.get(cache.lease(), History);
        QVERIFY(result); const auto output = result->rows.first().toObject();
        QVERIFY(!output.contains("access_token"));
        const auto encoded = output.value("payload").toObject();
        QCOMPARE(encoded.value("content").toString(), QString("encoded-ciphertext-fixture"));
        QVERIFY(!encoded.contains("private_key"));
        auto person = QJsonObject{{"uuid", User}, {"username", "cassi"}, {"first_name", "Cassandra"},
            {"token", "never-cache-this-fixture-token"}, {"credentials", QJsonObject{{"password", "fixture"}}}};
        NativeCache::Key users{NativeCache::Kind::Users, {}, {}, {}};
        QVERIFY(cache.put(cache.lease(), users, {person}));
        QCOMPARE(cache.get(cache.lease(), users)->rows.first().toObject().size(), 3);
        cache.close();
        QFile file(directory.filePath("cache.sqlite3")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QVERIFY(!bytes.contains("never-cache-this-fixture")); QVERIFY(!bytes.contains("credentials"));
        QVERIFY(bytes.contains("encoded-ciphertext-fixture"));
    }

    void invalidRowsAndCrossRouteDataCannotBeCached()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        auto row = message(); row.insert("topic_uuid", UserB);
        QVERIFY(!cache.put(cache.lease(), History, {row}));
        row = message(); row.insert("read", "false"); QVERIFY(!cache.put(cache.lease(), History, {row}));
        row = message(); QVERIFY(!cache.put(cache.lease(), History, {row, row}));
        auto channel = stream(); channel.insert("unread_count", -1);
        QVERIFY(!cache.put(cache.lease(), Streams, {channel}));
        QVERIFY(!cache.put(cache.lease(), {NativeCache::Kind::Topics, Stream, Topic, {}}, {}));
        QVERIFY(!cache.put(cache.lease(), {NativeCache::Kind::History, Stream, Topic, QByteArray("bad\0marker", 10)}, {}));
        QVERIFY(!cache.put(cache.lease(), History, {}, QByteArray(4097, 'x')));
        QVERIFY(cache.available()); QCOMPARE(cache.stats().entries, 0);
    }

    void cachedUserDisplayNameMatchesTheAuthoritativeDto()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        const NativeCache::Key users{NativeCache::Kind::Users, {}, {}, {}};
        const QJsonObject user{{"uuid", User}, {"username", "cassi"}, {"name", "Cassandra fixture"}};
        QVERIFY(cache.put(cache.lease(), users, {user}));
        QCOMPARE(cache.get(cache.lease(), users)->rows.first().toObject().value("name").toString(), QString("Cassandra fixture"));
    }

    void targetedInvalidationIncludesAllPaginationVariants()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User)); const auto lease = cache.lease();
        const NativeCache::Key older{NativeCache::Kind::History, Stream, Topic, "opaque='--page"};
        const NativeCache::Key other{NativeCache::Kind::History, Stream, UserB, {}};
        auto row = message(); auto otherRow = row; otherRow.insert("topic_uuid", UserB);
        QVERIFY(cache.put(lease, History, {row})); QVERIFY(cache.put(lease, older, {row}));
        QVERIFY(cache.put(lease, other, {otherRow})); QVERIFY(cache.put(lease, Streams, {stream()}));
        QVERIFY(cache.invalidate(lease, NativeCache::Kind::History, Stream, Topic));
        QVERIFY(!cache.get(lease, History)); QVERIFY(!cache.get(lease, older));
        QVERIFY(cache.get(lease, other)); QVERIFY(cache.get(lease, Streams));
        QCOMPARE(cache.stats().entries, 2);
    }

    void entryAndByteBudgetsEvictLeastRecentlyUsedAcrossOwners()
    {
        QTemporaryDir directory;
        NativeCache::Policy policy; policy.maxEntries = 2; policy.maxBytes = 4096; policy.maxEntryBytes = 2048;
        NativeCache cache(directory.path(), policy, false);
        QVERIFY(cache.open(Server, Project, User));
        auto lease = cache.lease();
        const NativeCache::Key older{NativeCache::Kind::History, Stream, Topic, "older"};
        QVERIFY(cache.put(lease, Streams, {stream()})); QVERIFY(cache.put(lease, History, {message()}));
        QVERIFY(cache.get(lease, Streams)); // Streams becomes most recently used.
        QVERIFY(cache.put(lease, older, {message()}));
        QVERIFY(cache.get(lease, Streams)); QVERIFY(!cache.get(lease, History));
        QVERIFY(cache.get(lease, older)); QCOMPARE(cache.stats().entries, 2);
        QVERIFY(cache.open(Server, Project, UserB)); lease = cache.lease();
        QVERIFY(cache.put(lease, Streams, {stream()})); QCOMPARE(cache.stats().entries, 2);
        QVERIFY(cache.open(Server, Project, User)); lease = cache.lease();
        QVERIFY(!cache.get(lease, Streams)); QVERIFY(cache.get(lease, older));
        QVERIFY(!cache.put(lease, History, {message(QString(2048, 'x'))}));
        QCOMPARE(cache.stats().entries, 2);
    }

    void totalBytesAndPhysicalJournalStayBounded()
    {
        QTemporaryDir directory;
        NativeCache::Policy policy; policy.maxEntries = 512; policy.maxBytes = 16 * 1024; policy.maxEntryBytes = 8 * 1024;
        NativeCache cache(directory.path(), policy, false); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease();
        for (int i = 0; i < 200; ++i) {
            NativeCache::Key key = History; key.marker = QByteArray::number(i);
            QVERIFY(cache.put(lease, key, {message(QString(2000, 'a'))}));
            QVERIFY(cache.stats().bytes <= policy.maxBytes);
        }
        QVERIFY(cache.stats().entries <= 6);
        // Freed SQLite pages and its journal are reused, rather than growing for
        // every navigation or account. Allow fixed SQLite page/index overhead.
        QVERIFY(QFileInfo(directory.filePath("cache.sqlite3")).size() < 256 * 1024);
        QVERIFY(QFileInfo(directory.filePath("cache.sqlite3-wal")).size() <= 1048576);
    }

    void injectedSmallBudgetRemainsBoundedUnderLargeHistoryPages()
    {
        QTemporaryDir directory;
        NativeCache::Policy policy; policy.maxBytes = 32 * 1024 * 1024; policy.maxEntries = 512;
        NativeCache cache(directory.path(), policy, false); QVERIFY(cache.open(Server, Project, User));
        const auto lease = cache.lease();
        const auto largeRow = message(QString(1024 * 1024, 'a'));
        for (int i = 0; i < 64; ++i) {
            auto key = History; key.marker = QByteArray::number(i);
            QVERIFY(cache.put(lease, key, {largeRow}));
        }
        QVERIFY(cache.stats().bytes <= 32 * 1024 * 1024);
        QVERIFY(cache.stats().entries < 32);
        auto oldest = History; oldest.marker = "0";
        auto newest = History; newest.marker = "63";
        QVERIFY(!cache.get(lease, oldest)); QVERIFY(cache.get(lease, newest));
        QVERIFY(QFileInfo(directory.filePath("cache.sqlite3")).size() <= 64 * 1024 * 1024);
        QVERIFY(QFileInfo(directory.filePath("cache.sqlite3-wal")).size() <= 1048576);
        qInfo().nospace() << "64 x 1MiB writes: retained=" << cache.stats().entries << " pages, payload="
            << cache.stats().bytes << " bytes, database=" << QFileInfo(directory.filePath("cache.sqlite3")).size() << " bytes";
    }

    void privatePermissionsSymlinkAndCorruptDatabaseFailClosed()
    {
        QTemporaryDir directory;
        const auto subdir = directory.filePath("cache");
        NativeCache cache(subdir); QVERIFY(cache.open(Server, Project, User));
        QVERIFY(cache.put(cache.lease(), Streams, {stream()}));
#ifdef Q_OS_UNIX
        const auto publicBits = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QVERIFY(!(QFileInfo(subdir).permissions() & publicBits));
        for (const auto *name : {"cache.sqlite3", "cache.sqlite3-wal", "cache.sqlite3-shm"})
            QVERIFY(!(QFileInfo(subdir + '/' + name).permissions() & publicBits));
#endif
        cache.close();
        QFile file(subdir + "/cache.sqlite3"); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("corrupt database fixture"), qint64(24)); file.close();
        QVERIFY(!cache.open(Server, Project, User)); QVERIFY(!cache.available()); QVERIFY(!cache.error().isEmpty());
        NativeCache link(directory.filePath("link"));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(subdir, directory.filePath("link")));
        QVERIFY(!link.open(Server, Project, User));
#endif
    }

    void modifiedDiskRowsAreRevalidatedAndFutureSchemaIsNotAdopted()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        QVERIFY(cache.put(cache.lease(), Streams, {stream()})); cache.close();
        QVERIFY(modify(directory.path(), "UPDATE entries SET payload='[{\"uuid\":\"invalid\"}]'"));
        QVERIFY(cache.open(Server, Project, User)); QVERIFY(!cache.get(cache.lease(), Streams));
        cache.close(); QVERIFY(modify(directory.path(), "PRAGMA user_version=999"));
        QVERIFY(!cache.open(Server, Project, User));
    }

    void anUnrelatedDatabaseIsRejectedBeforeChangingItsJournalOrContents()
    {
        QTemporaryDir directory;
        QVERIFY(modify(directory.path(), "CREATE TABLE unrelated(value TEXT)"));
        QFile file(directory.filePath("cache.sqlite3")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto before = file.readAll(); file.close();
        NativeCache cache(directory.path()); QVERIFY(!cache.open(Server, Project, User));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), before);
    }

    void corruptSizeMetadataCannotBypassThePayloadBudget()
    {
        QTemporaryDir directory;
        NativeCache::Policy policy; policy.maxBytes = 4096; policy.maxEntryBytes = 4096;
        NativeCache cache(directory.path(), policy, false);
        QVERIFY(cache.open(Server, Project, User));
        QVERIFY(cache.put(cache.lease(), History, {message(QString(2400, 'a'))}));
        cache.close(); QVERIFY(modify(directory.path(), "UPDATE entries SET size=1"));
        QVERIFY(cache.open(Server, Project, User));
        QVERIFY(cache.stats().bytes > 2400);
        auto older = History; older.marker = "older";
        QVERIFY(cache.put(cache.lease(), older, {message(QString(2400, 'b'))}));
        QCOMPARE(cache.stats().entries, 1);
        QVERIFY(!cache.get(cache.lease(), History));
        QVERIFY(cache.get(cache.lease(), older));
    }

    void unsafeOriginsAreRejectedAndLoopbackNeedsExplicitTestOptIn()
    {
        QTemporaryDir directory;
        NativeCache cache(directory.path());
        for (const auto *url : {"http://workspace.example.test", "https://user:password@workspace.example.test",
                "https://workspace.example.test/path", "https://workspace.example.test/?token=fixture",
                "https://workspace.example.test/#fragment", "http://127.0.0.1:8137"})
            QVERIFY(!cache.open(QUrl(QString::fromLatin1(url)), Project, User));
        NativeCache fixture(directory.path(), NativeCache::Policy{}, true);
        QVERIFY(fixture.open(QUrl("http://127.0.0.1:8137"), Project, User));
        QVERIFY(!fixture.open(QUrl("http://remote.example.test"), Project, User));
    }

    void measuredHistoryPageReadAndWrite()
    {
        QTemporaryDir directory;
        QJsonArray rows;
        for (int i = 0; i < 50; ++i) rows.append(message(QString(200, 'a') + QString::number(i)));
        NativeCache cache(directory.path()); QVERIFY(cache.open(Server, Project, User));
        QElapsedTimer timer; timer.start();
        QVERIFY(cache.put(cache.lease(), History, rows));
        const auto writeMicros = timer.nsecsElapsed() / 1000;
        timer.restart();
        for (int i = 0; i < 100; ++i) {
            const auto page = cache.get(cache.lease(), History); QVERIFY(page && page->fresh); QCOMPARE(page->rows.size(), 50);
        }
        const auto readMicros = timer.nsecsElapsed() / 1000 / 100;
        qInfo().nospace() << "50 messages: write=" << writeMicros << "us, mean read=" << readMicros
                         << "us, payload=" << cache.stats().bytes << " bytes";
        // Measurement only; machine load does not become a flaky latency gate.
        QCOMPARE(cache.stats().hits, quint64(100)); QCOMPARE(cache.stats().staleHits, quint64(0));
    }
};

QTEST_GUILESS_MAIN(NativeCacheTest)
#include "native-cache-test.moc"
