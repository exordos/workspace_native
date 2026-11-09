#include "native-draft-store.h"
#include "mock-server.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <cstdlib>
#include <algorithm>

using namespace workspace;
using namespace workspace::test;
using Failure = NativeDraftStore::Failure;

namespace {
const QString ownerA(64, 'a'), ownerB(64, 'b');
DraftRecord record(const QString &id = returnedId)
{
    DraftRecord r;
    r.uuid = id; r.stream = streamId; r.topic = topicId; r.snapshot = messageId;
    r.text = "**Unsent draft**\n  exact spacing 😀\n"; r.updatedAt = 1760000000123;
    r.replySession = QJsonObject{{"tabs", QJsonArray{QJsonObject{{"id", "reply-tab"}, {"messageUuid", messageId},
        {"quotedContent", "Quoted\ncontent"}, {"answer", "Unsent reply"}}}}, {"activeTabId", "reply-tab"}};
    return r;
}

bool sql(const QString &path, const QString &statement)
{
    const QString name = "cassi-draft-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
        if (db.open()) { QSqlQuery query(db); ok = query.exec(statement); }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}
}

class NativeDraftStoreTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }

    void exactTextReplyAndSyncIntentSurviveCloseAndReopen()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        auto expected = record(); expected.createPending = true; expected.createText = "original POST";
        expected.serverUpdatedAt = "2026-01-02T10:00:01.123456Z";
        expected.text = "new input after the first POST"; expected.phase = DraftRecord::Phase::Saving;
        {
            NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
            QCOMPARE(store.put(expected), Failure::None);
        }
        NativeDraftStore reopened(directory.path()); QCOMPARE(reopened.open(ownerA), Failure::None);
        auto result = reopened.read(expected.uuid); QCOMPARE(result.failure, Failure::None); QVERIFY(result.record);
        const auto &r = *result.record;
        QCOMPARE(r.text, expected.text); QCOMPARE(r.createText, expected.createText); QVERIFY(r.createPending);
        QCOMPARE(r.snapshot, expected.snapshot); QCOMPARE(r.replySession, expected.replySession);
        QCOMPARE(r.stream, streamId); QCOMPARE(r.topic, topicId); QCOMPARE(r.updatedAt, expected.updatedAt);
        QCOMPARE(r.serverUpdatedAt, expected.serverUpdatedAt);
        QCOMPARE(r.phase, DraftRecord::Phase::Saving);
        const auto page = reopened.list(); QCOMPARE(page.failure, Failure::None); QCOMPARE(page.rows.size(), 1);
        QCOMPARE(page.rows.first().uuid, expected.uuid); QVERIFY(page.next.isEmpty());
        const auto explicitEmpty = reopened.list(QStringLiteral(""));
        QCOMPARE(explicitEmpty.failure, Failure::None); QCOMPARE(explicitEmpty.rows.size(), 1);
        QCOMPARE(explicitEmpty.rows.first().uuid, page.rows.first().uuid);
    }

    void formatOneWithoutOptionalRemoteMetadataStillReadsExactly()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        const auto expected = record(); QCOMPARE(store.put(expected), Failure::None); store.close();
        QVERIFY(sql(directory.filePath(ownerA + ".sqlite3"),
            "UPDATE draft_records SET payload=CAST(json_remove(payload,'$.serverUpdatedAt','$.privateDeletion','$.privateCreateUncertain') AS BLOB)"));
        QCOMPARE(store.open(ownerA), Failure::None); const auto result = store.read(expected.uuid); QVERIFY(result.record);
        QCOMPARE(result.record->text, expected.text); QCOMPARE(result.record->updatedAt, expected.updatedAt);
        QVERIFY(result.record->serverUpdatedAt.isEmpty()); QVERIFY(!result.record->privateDeletion); QVERIFY(!result.record->privateCreateUncertain);
    }

    void metadataOnlyPrivateDeletionSurvivesReopen()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        auto expected = record(); expected.text.clear(); expected.replySession = {};
        expected.consumed = true; expected.phase = DraftRecord::Phase::Deleting;
        expected.privateDeletion = true; expected.privateCreateUncertain = true;
        QCOMPARE(store.put(expected), Failure::None); store.close(); QCOMPARE(store.open(ownerA), Failure::None);
        const auto result = store.read(expected.uuid); QVERIFY(result.record);
        QVERIFY(result.record->privateDeletion); QVERIFY(result.record->privateCreateUncertain);
        QVERIFY(result.record->text.isEmpty()); QVERIFY(result.record->createText.isEmpty()); QVERIFY(result.record->replySession.isEmpty());
        QVERIFY(!store.latest(streamId, topicId).record);
    }

    void failedPrivateDeletionKeepsOnlyRetryMetadata()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        auto r = record(); r.text.clear(); r.replySession = {}; r.consumed = true;
        r.privateDeletion = true; r.revision = 4; r.phase = DraftRecord::Phase::Failed;
        QCOMPARE(store.put(r), Failure::None); store.close(); QCOMPARE(store.open(ownerA), Failure::None);
        const auto loaded = store.read(r.uuid); QVERIFY(loaded.record); QVERIFY(loaded.record->privateDeletion);
        QCOMPARE(loaded.record->phase, DraftRecord::Phase::Failed); QVERIFY(loaded.record->text.isEmpty());
        QVERIFY(!store.latest(streamId, topicId).record);
    }

    void previewPagesAreBoundedOrderedAndDoNotRetainFullBodies()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QVector<DraftRecord> input;
        for (int i = 0; i < 205; ++i) {
            auto r = record(QUuid::createUuid().toString(QUuid::WithoutBraces));
            r.updatedAt = i / 3; r.text = "  line\n\t" + QString(153, 'x') + QString::fromUtf8("😀") + QString(1000, 'y');
            r.consumed = i % 4 == 0; input.append(r);
        }
        for (int i = 0; i < input.size(); i += 100) QCOMPARE(store.apply(input.mid(i, 100), {}), Failure::None);
        std::sort(input.begin(), input.end(), [](const auto &a, const auto &b) {
            return a.updatedAt == b.updatedAt ? a.uuid > b.uuid : a.updatedAt > b.updatedAt;
        });
        QSet<QString> ids; QString cursor; int offset = 0;
        do {
            const auto page = store.previews(cursor); QCOMPARE(page.failure, Failure::None);
            QCOMPARE(page.rows.size(), qMin(100, 205-offset));
            for (const auto &r : page.rows) {
                QCOMPARE(r.uuid, input[offset].uuid); QCOMPARE(r.snapshot, input[offset].snapshot);
                QCOMPARE(r.consumed, input[offset].consumed); QVERIFY(!ids.contains(r.uuid)); ids.insert(r.uuid); ++offset;
                QVERIFY(r.text.size() <= 161); QVERIFY(r.text.endsWith(QChar(0x2026)));
                QVERIFY(!r.text.at(r.text.size()-2).isHighSurrogate()); QVERIFY(!r.text.contains('\n')); QVERIFY(!r.text.contains('\t'));
            }
            cursor = page.next;
        } while (!cursor.isEmpty());
        QCOMPARE(offset, 205); QCOMPARE(ids.size(), 205);
        for (const auto &bad : QStringList{"bad", "01/"+returnedId, "-1/"+returnedId, "1/not-a-uuid"})
            QCOMPARE(store.previews(bad).failure, Failure::InvalidRecord);
        QCOMPARE(store.read(input.first().uuid).record->text, input.first().text);
    }

    void authoritativeStreamsRemoveOnlyInaccessibleOwnerData()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        auto allowed = record(), removed = record(messageId); removed.stream = secondTopicId;
        QCOMPARE(store.apply({allowed, removed}, {}), Failure::None);
        QCOMPARE(store.retainStreams({"invalid"}), Failure::InvalidRecord); QVERIFY(store.read(removed.uuid).record);
        QCOMPARE(store.retainStreams({streamId}), Failure::None); QVERIFY(store.read(allowed.uuid).record); QVERIFY(!store.read(removed.uuid).record);
        store.close(); QCOMPARE(store.open(ownerB), Failure::None); QCOMPARE(store.put(removed), Failure::None);
        store.close(); QCOMPARE(store.open(ownerA), Failure::None); QCOMPARE(store.retainStreams({}), Failure::None);
        QVERIFY(store.list().rows.isEmpty()); store.close(); QCOMPARE(store.open(ownerB), Failure::None); QVERIFY(store.read(removed.uuid).record);
    }

    void invalidRemoteMetadataCannotReplaceAnExistingRecord_data()
    {
        QTest::addColumn<int>("mutation");
        for (int i = 0; i < 5; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidRemoteMetadataCannotReplaceAnExistingRecord()
    {
        QFETCH(int, mutation); QTemporaryDir directory; NativeDraftStore store(directory.path());
        QCOMPARE(store.open(ownerA), Failure::None); const auto original = record(); QCOMPARE(store.put(original), Failure::None);
        auto next = original;
        switch (mutation) {
        case 0: next.serverUpdatedAt = "not-a-server-time"; break;
        case 1: next.privateCreateUncertain = true; break;
        case 2: next.privateDeletion = true; break;
        case 3: next.privateDeletion = true; next.consumed = true; next.phase = DraftRecord::Phase::Deleting; break;
        case 4:
            next.privateDeletion = next.privateCreateUncertain = next.consumed = true;
            next.phase = DraftRecord::Phase::Deleting; next.text.clear(); next.replySession = {}; next.revision = 3; break;
        }
        QCOMPARE(store.put(next), Failure::InvalidRecord); QCOMPARE(store.read(original.uuid).record->text, original.text);
    }

    void latestUsesAnIndexedConversationAndIgnoresConsumedAndDeletingRows()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        auto a = record(messageId), b = record(returnedId), c = record(secondTopicId), d = record(topicId);
        a.updatedAt = b.updatedAt = 10; c.updatedAt = 20; d.updatedAt = 30;
        c.consumed = true; d.phase = DraftRecord::Phase::Deleting;
        QCOMPARE(store.apply({a, b, c, d}, {}), Failure::None);
        auto result = store.latest(streamId, topicId); QCOMPARE(result.failure, Failure::None); QVERIFY(result.record);
        QCOMPARE(result.record->uuid, returnedId); // Deterministic UUID tie-break.
        result = store.latest(streamId, secondTopicId); QCOMPARE(result.failure, Failure::None); QVERIFY(!result.record);
        QCOMPARE(store.latest("bad", topicId).failure, Failure::InvalidRecord);
    }

    void committedWalSurvivesARealProcessWithoutDestructors()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {"--draft-store-write-and-exit", directory.path()});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), 0);
        const auto diagnostics = process.readAllStandardError();
        QVERIFY2(diagnostics.isEmpty(), diagnostics.constData());
        NativeDraftStore reopened(directory.path()); QCOMPARE(reopened.open(ownerA), Failure::None);
        const auto loaded = reopened.read(returnedId); QCOMPARE(loaded.failure, Failure::None); QVERIFY(loaded.record);
        QCOMPARE(loaded.record->text, record().text); QCOMPARE(loaded.record->replySession, record().replySession);
    }

    void ownerFilesAndLocksDoNotCrossAccounts()
    {
        QTemporaryDir directory;
        NativeDraftStore first(directory.path()), second(directory.path());
        QCOMPARE(first.open(ownerA), Failure::None); QCOMPARE(first.put(record()), Failure::None);
        QCOMPARE(second.open(ownerA), Failure::Locked); QVERIFY(!second.available());
        QCOMPARE(second.open(ownerB), Failure::None); QVERIFY(!second.read(returnedId).record);
        auto other = record(); other.text = "other account"; QCOMPARE(second.put(other), Failure::None);
        QCOMPARE(first.read(returnedId).record->text, record().text);
        first.close(); QCOMPARE(first.open(ownerB), Failure::Locked);
        second.close(); QCOMPARE(first.open(ownerB), Failure::None);
        QCOMPARE(first.read(returnedId).record->text, other.text);
    }

    void renamedOwnerDatabaseIsRejectedWithoutImportingItsDrafts()
    {
        QTemporaryDir directory;
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.put(record()), Failure::None); store.close();
        QVERIFY(QFile::copy(directory.filePath(ownerA + ".sqlite3"), directory.filePath(ownerB + ".sqlite3")));
        QCOMPARE(store.open(ownerB), Failure::Corrupt); QVERIFY(!store.available());
        QCOMPARE(store.open(ownerA), Failure::None); QVERIFY(store.read(returnedId).record);
    }

    void ownerValidation_data()
    {
        QTest::addColumn<QString>("owner");
        QTest::newRow("empty") << QString{};
        QTest::newRow("short") << QString(63, 'a');
        QTest::newRow("uppercase") << QString(64, 'A');
        QTest::newRow("non-hash") << QString(64, 'x');
        QTest::newRow("path") << QString("../") + QString(64, 'a');
    }
    void ownerValidation()
    {
        QFETCH(QString, owner); QTemporaryDir directory;
        NativeDraftStore store(directory.filePath("not-created"));
        QCOMPARE(store.open(owner), Failure::InvalidOwner); QVERIFY(!store.available());
        QVERIFY(!QFileInfo::exists(directory.filePath("not-created")));
    }

    void deletingAndConflictSnapshotsRemainDistinctAcrossRestart()
    {
        QTemporaryDir directory;
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        auto consumed = record(); consumed.phase = DraftRecord::Phase::Deleting; consumed.consumed = true; consumed.revision = 7;
        auto conflict = record(messageId); conflict.phase = DraftRecord::Phase::Conflict;
        conflict.revision = 3; conflict.conflictRevision = 4; conflict.conflictText = "other client's text";
        QCOMPARE(store.apply({consumed, conflict}, {}), Failure::None); store.close();
        QCOMPARE(store.open(ownerA), Failure::None);
        const auto a = store.read(returnedId), b = store.read(messageId); QVERIFY(a.record); QVERIFY(b.record);
        QVERIFY(a.record->consumed); QCOMPARE(a.record->phase, DraftRecord::Phase::Deleting); QCOMPARE(a.record->revision, 7);
        QVERIFY(!b.record->consumed); QCOMPARE(b.record->text, conflict.text);
        QCOMPARE(b.record->conflictText, conflict.conflictText); QCOMPARE(b.record->conflictRevision, 4);
    }

    void protectedOrMalformedInputCannotReplaceDurablePublicText_data()
    {
        QTest::addColumn<int>("mutation");
        for (int i = 0; i < 9; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void protectedOrMalformedInputCannotReplaceDurablePublicText()
    {
        QFETCH(int, mutation); QTemporaryDir directory;
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None); QCOMPARE(store.put(record()), Failure::None);
        auto next = record();
        switch (mutation) {
        case 0: next.text = ":::private plaintext"; break;
        case 1: next.createText = ":::private first POST"; next.createPending = true; break;
        case 2: next.conflictText = ":::private server text"; break;
        case 3: next.replySession = QJsonObject{{"tabs", QJsonArray{QJsonObject{{"selectedText", ":::private selection"}}}}}; break;
        case 4: next.text = QString(40001, 'x'); break;
        case 5: next.text = QString(QChar(0xd800)); break;
        case 6: next.phase = DraftRecord::Phase::Saved; next.revision = 0; break;
        case 7: next.snapshot = "bad"; break;
        case 8: next.phase = DraftRecord::Phase::Conflict; next.conflictRevision = 0; break;
        }
        QCOMPARE(store.put(next), Failure::InvalidRecord);
        const auto old = store.read(returnedId); QVERIFY(old.record); QCOMPARE(old.record->text, record().text);
    }

    void batchValidationAndSqlFailureLeaveEveryPreviousRecordIntact()
    {
        QTemporaryDir directory;
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None); QCOMPARE(store.put(record()), Failure::None);
        auto changed = record(); changed.text = "changed";
        auto invalid = record(messageId); invalid.topic = "invalid";
        QCOMPARE(store.apply({changed, invalid}, {}), Failure::InvalidRecord);
        QCOMPARE(store.read(returnedId).record->text, record().text);
        QVERIFY(sql(directory.filePath(ownerA + ".sqlite3"), "CREATE TRIGGER abort_test BEFORE INSERT ON draft_records "
            "WHEN NEW.uuid = '" + messageId + "' BEGIN SELECT RAISE(ABORT, 'synthetic write failure'); END"));
        QCOMPARE(store.apply({changed, record(messageId)}, {}), Failure::Unavailable);
        QCOMPARE(store.read(returnedId).record->text, record().text); QVERIFY(!store.read(messageId).record);
    }

    void metadataPagesAreBoundedAndNeverSkipOrRepeatRecords()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QStringList expected;
        for (int i = 0; i < 107; ++i) {
            auto r = record(QUuid::createUuid().toString(QUuid::WithoutBraces)); expected.append(r.uuid);
            QCOMPARE(store.put(r), Failure::None);
        }
        expected.sort(); QStringList actual; QString cursor; int pages = 0;
        do {
            const auto page = store.list(cursor); QCOMPARE(page.failure, Failure::None); QVERIFY(page.rows.size() <= 100);
            for (const auto &row : page.rows) actual.append(row.uuid);
            cursor = page.next; ++pages;
        } while (!cursor.isEmpty() && pages < 3);
        QCOMPARE(pages, 2); QCOMPARE(actual, expected);
        QCOMPARE(store.list({}, 101).failure, Failure::InvalidRecord);
        QCOMPARE(store.list("bad").failure, Failure::InvalidRecord);
    }

    void transactionCapacityAndDuplicateIdsAreExplicitFailures()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.apply({record(), record()}, {}), Failure::InvalidRecord);
        QCOMPARE(store.apply({record()}, {returnedId}), Failure::InvalidRecord);
        QVector<DraftRecord> rows;
        for (int i = 0; i < 101; ++i) rows.append(record(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QCOMPARE(store.apply(rows, {}), Failure::Capacity);
        rows.clear();
        for (int i = 0; i < 3; ++i) {
            auto r = record(QUuid::createUuid().toString(QUuid::WithoutBraces));
            r.replySession = QJsonObject{{"ordinary-context", QString(3 * 1024 * 1024, 'x')}}; rows.append(r);
        }
        QCOMPARE(store.apply(rows, {}), Failure::Capacity); QVERIFY(store.list().rows.isEmpty());
    }

    void corruptOrFutureSchemaDoesNotBecomeAnEmptySuccessfulStore()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.put(record()), Failure::None);
        QVERIFY(sql(directory.filePath(ownerA + ".sqlite3"), "UPDATE draft_records SET payload = X'6e6f742d6a736f6e'"));
        QCOMPARE(store.read(returnedId).failure, Failure::Corrupt); store.close();
        QVERIFY(sql(directory.filePath(ownerA + ".sqlite3"), "PRAGMA user_version=2"));
        QCOMPARE(store.open(ownerA), Failure::Schema); QVERIFY(!store.available());
    }

    void missingRecordTableCannotReportAnAvailableStore()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.put(record()), Failure::None); store.close();
        QVERIFY(sql(directory.filePath(ownerA + ".sqlite3"), "DROP TABLE draft_records"));
        QCOMPARE(store.open(ownerA), Failure::Corrupt); QVERIFY(!store.available());
    }

    void corruptMetadataCannotBeCoercedIntoAValidSummary_data()
    {
        QTest::addColumn<QString>("assignment");
        QTest::newRow("fractional-time") << QString("updated_at = 1.5");
        QTest::newRow("text-time") << QString("updated_at = 'invalid'");
        QTest::newRow("fractional-phase") << QString("phase = 1.5");
        QTest::newRow("invalid-consumed") << QString("consumed = 2");
    }
    void corruptMetadataCannotBeCoercedIntoAValidSummary()
    {
        QFETCH(QString, assignment); QTemporaryDir directory;
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.put(record()), Failure::None);
        QVERIFY(sql(directory.filePath(ownerA + ".sqlite3"), "UPDATE draft_records SET " + assignment));
        QCOMPARE(store.list().failure, Failure::Corrupt); QCOMPARE(store.read(returnedId).failure, Failure::Corrupt);
        QCOMPARE(store.latest(streamId, topicId).failure, Failure::Corrupt);
    }

    void removalAndPrivateFilePermissionsSurviveReopen()
    {
        QTemporaryDir directory; NativeDraftStore store(directory.filePath("drafts")); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.put(record()), Failure::None);
        const auto privateBits = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        for (const auto &suffix : {QString{}, QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            const QFileInfo file(directory.filePath("drafts/" + ownerA + ".sqlite3" + suffix));
            if (file.exists()) QCOMPARE(file.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther), QFileDevice::Permissions{});
            if (file.exists()) QCOMPARE(file.permissions() & privateBits, privateBits);
        }
        QCOMPARE(store.remove(returnedId), Failure::None); QCOMPARE(store.remove(returnedId), Failure::None);
        store.close(); QCOMPARE(store.open(ownerA), Failure::None);
        const auto absent = store.read(returnedId); QCOMPARE(absent.failure, Failure::None); QVERIFY(!absent.record);
        QVERIFY(store.list().rows.isEmpty());
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--draft-store-write-and-exit") {
        NativeDraftStore store(app.arguments()[2]);
        if (store.open(ownerA) != Failure::None || store.put(record()) != Failure::None) return 3;
        std::_Exit(0); // Preserve a real committed WAL; do not run object destructors.
    }
    NativeDraftStoreTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "native-draft-store-test.moc"
