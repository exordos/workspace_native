#include "native-draft-service.h"
#include "mock-server.h"

#include <QPointer>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

using namespace workspace;
using namespace workspace::test;
using Failure = NativeDraftService::Failure;

namespace {
const QString ownerA(64, 'a'), ownerB(64, 'b');
DraftRecord record(const QString &id = returnedId)
{
    DraftRecord r; r.uuid = id; r.stream = streamId; r.topic = topicId; r.snapshot = messageId;
    r.text = "Durable draft"; r.updatedAt = 1000;
    return r;
}
class WriterLock final {
public:
    QString name = "cassi-draft-lock-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db;
    bool acquire(const QString &path)
    {
        db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
        if (!db.open()) return false;
        QSqlQuery query(db); return query.exec("BEGIN IMMEDIATE");
    }
    void release() { QSqlQuery query(db); query.exec("COMMIT"); }
    ~WriterLock() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
};
}

class NativeDraftServiceTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }

    void actualWorkerWritesReadsAndListsOwnedRecords()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path());
        QVERIFY(service.open(ownerA)); const auto lease = service.lease();
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000);
        QCOMPARE(service.failure(), Failure::None);
        bool written = false; Failure writeResult = Failure::Unavailable;
        QVERIFY(service.apply(lease, {record()}, {}, this, [&](Failure f) { writeResult = f; written = true; }));
        QTRY_VERIFY_WITH_TIMEOUT(written, 2000); QCOMPARE(writeResult, Failure::None);
        NativeDraftStore::Read loaded; bool read = false;
        QVERIFY(service.read(lease, returnedId, this, [&](auto r) { loaded = r; read = true; }));
        QTRY_VERIFY_WITH_TIMEOUT(read, 2000); QCOMPARE(loaded.failure, Failure::None); QVERIFY(loaded.record);
        QCOMPARE(loaded.record->text, record().text);
        bool listed = false; NativeDraftStore::Page page;
        QVERIFY(service.list(lease, {}, this, [&](auto r) { page = r; listed = true; }));
        QTRY_VERIFY_WITH_TIMEOUT(listed, 2000); QCOMPARE(page.failure, Failure::None); QCOMPARE(page.rows.size(), 1);
        QCOMPARE(service.pendingDataJobs(), 0); QCOMPARE(service.pendingBytes(), 0);
        service.shutdown(); QTRY_VERIFY_WITH_TIMEOUT(service.stopped(), 2000); QVERIFY(!service.available());
    }

    void latestRestorableConversationUsesTimeUuidAndConsumedState()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        auto old = record(messageId), recent = record(returnedId), consumed = record(secondTopicId), deleting = record(topicId);
        old.updatedAt = 10; recent.updatedAt = 20; consumed.updatedAt = 30; consumed.consumed = true;
        deleting.updatedAt = 40; deleting.phase = DraftRecord::Phase::Deleting;
        bool saved = false; QVERIFY(service.apply(lease, {old, recent, consumed, deleting}, {}, this, [&](Failure f) { saved = f == Failure::None; }));
        QTRY_VERIFY_WITH_TIMEOUT(saved, 2000);
        bool loaded = false; NativeDraftStore::Read result;
        QVERIFY(service.latest(lease, streamId, topicId, this, [&](auto r) { result = r; loaded = true; }));
        QTRY_VERIFY_WITH_TIMEOUT(loaded, 2000); QCOMPARE(result.failure, Failure::None); QVERIFY(result.record);
        QCOMPARE(result.record->uuid, returnedId);
        bool absent = false; QVERIFY(service.latest(lease, streamId, secondTopicId, this, [&](auto r) {
            absent = r.failure == Failure::None && !r.record;
        })); QTRY_VERIFY_WITH_TIMEOUT(absent, 2000);
    }

    void acceptedWriteKeepsItsOwnerWhenTheForegroundAccountChanges()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto oldLease = service.lease();
        bool saved = false; QVERIFY(service.apply(oldLease, {record()}, {}, this, [&](Failure f) { saved = f == Failure::None; }));
        QVERIFY(service.open(ownerB)); const auto fresh = service.lease();
        QVERIFY(fresh.generation != oldLease.generation);
        QTRY_VERIFY_WITH_TIMEOUT(service.available() && saved, 2000);
        bool absent = false; QVERIFY(service.read(fresh, returnedId, this, [&](auto r) {
            absent = r.failure == Failure::None && !r.record;
        })); QTRY_VERIFY_WITH_TIMEOUT(absent, 2000);
        bool rejected = false; QVERIFY(!service.read(oldLease, returnedId, this, [&](auto r) {
            rejected = r.failure == Failure::Unavailable && !r.record;
        })); QTRY_VERIFY_WITH_TIMEOUT(rejected, 2000);
        service.shutdown(); QTRY_VERIFY_WITH_TIMEOUT(service.stopped(), 2000);
        NativeDraftStore a(directory.path()); QCOMPARE(a.open(ownerA), Failure::None);
        QVERIFY(a.read(returnedId).record);
    }

    void anAlreadyRunningOldReadCannotPopulateANewOwner()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        bool saved = false; QVERIFY(service.apply(lease, {record()}, {}, this, [&](Failure f) { saved = f == Failure::None; }));
        QTRY_VERIFY_WITH_TIMEOUT(saved, 2000);
        bool settled = false; NativeDraftStore::Read result;
        QVERIFY(service.read(lease, returnedId, this, [&](auto r) { result = r; settled = true; }));
        QVERIFY(service.open(ownerB));
        QTRY_VERIFY_WITH_TIMEOUT(settled && service.available(), 2000);
        QCOMPARE(result.failure, Failure::Unavailable); QVERIFY(!result.record);
    }

    void sqliteWriterContentionDoesNotBlockTheGuiEventLoop()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        WriterLock lock; QVERIFY(lock.acquire(directory.filePath(ownerA + ".sqlite3")));
        int pulses = 0; bool settled = false; Failure result = Failure::Unavailable;
        QTimer pulse; pulse.setInterval(10); connect(&pulse, &QTimer::timeout, this, [&] {
            if (++pulses == 12) lock.release();
        }); pulse.start();
        QVERIFY(service.apply(lease, {record()}, {}, this, [&](Failure f) { result = f; settled = true; }));
        QTRY_VERIFY_WITH_TIMEOUT(settled, 3000); pulse.stop();
        QVERIFY(pulses >= 12); QCOMPARE(result, Failure::None);
    }

    void boundedAdmissionRejectsExtraJobsWithoutDroppingAcceptedWrites()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        WriterLock lock; QVERIFY(lock.acquire(directory.filePath(ownerA + ".sqlite3")));
        int writes = 0;
        for (int i = 0; i < NativeDraftService::MaximumDataJobs; ++i) {
            auto r = record(QUuid::createUuid().toString(QUuid::WithoutBraces));
            QVERIFY(service.apply(lease, {r}, {}, this, [&](Failure f) { if (f == Failure::None) ++writes; }));
        }
        QCOMPARE(service.pendingDataJobs(), NativeDraftService::MaximumDataJobs);
        QVERIFY(service.pendingBytes() <= NativeDraftService::MaximumPendingBytes);
        bool rejected = false;
        QVERIFY(!service.apply(lease, {record()}, {}, this, [&](Failure f) { rejected = f == Failure::Capacity; }));
        int barriers = 0;
        QVERIFY(service.flush(this, [&](Failure f) { if (f == Failure::None) ++barriers; }));
        QVERIFY(service.flush(this, [&](Failure f) { if (f == Failure::None) ++barriers; }));
        bool extraBarrier = false;
        QVERIFY(!service.flush(this, [&](Failure f) { extraBarrier = f == Failure::Capacity; }));
        lock.release();
        QTRY_VERIFY_WITH_TIMEOUT(writes == NativeDraftService::MaximumDataJobs && barriers == 2 && rejected && extraBarrier, 3000);
        bool listed = false; int rows = 0;
        QVERIFY(service.list(lease, {}, this, [&](auto p) { listed = p.failure == Failure::None; rows = p.rows.size(); }));
        QTRY_VERIFY_WITH_TIMEOUT(listed, 2000); QCOMPARE(rows, NativeDraftService::MaximumDataJobs);
        QCOMPARE(service.pendingBytes(), 0);
    }

    void destructorDrainsAcceptedWritesWithoutDependingOnQueuedGuiCallbacks()
    {
        QTemporaryDir directory;
        {
            NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
            QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
            for (const auto &id : {messageId, returnedId, secondTopicId}) QVERIFY(service.apply(lease, {record(id)}, {}));
        }
        NativeDraftStore reopened(directory.path()); QCOMPARE(reopened.open(ownerA), Failure::None);
        const auto page = reopened.list(); QCOMPARE(page.failure, Failure::None); QCOMPARE(page.rows.size(), 3);
    }

    void serializedByteBudgetRejectsBeforeTheJobCountLimit()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        WriterLock lock; QVERIFY(lock.acquire(directory.filePath(ownerA + ".sqlite3")));
        const QJsonObject context{{"answer", QString(3 * 1024 * 1024, 'x')}};
        int written = 0;
        for (int i = 0; i < 5; ++i) {
            auto a = record(QUuid::createUuid().toString(QUuid::WithoutBraces));
            auto b = record(QUuid::createUuid().toString(QUuid::WithoutBraces)); a.replySession = b.replySession = context;
            QVERIFY(service.apply(lease, {a, b}, {}, this, [&](Failure f) { if (f == Failure::None) ++written; }));
        }
        QCOMPARE(service.pendingDataJobs(), 5); QVERIFY(service.pendingDataJobs() < NativeDraftService::MaximumDataJobs);
        QVERIFY(service.pendingBytes() <= NativeDraftService::MaximumPendingBytes);
        auto a = record(messageId), b = record(returnedId); a.replySession = b.replySession = context;
        bool capacity = false;
        QVERIFY(!service.apply(lease, {a, b}, {}, this, [&](Failure f) { capacity = f == Failure::Capacity; }));
        lock.release(); QTRY_VERIFY_WITH_TIMEOUT(written == 5 && capacity, 10000);
        QCOMPARE(service.pendingBytes(), 0);
    }

    void stateObserverDestructionStillDrainsTheNextAcceptedWrite()
    {
        QTemporaryDir directory; QPointer<NativeDraftService> service = new NativeDraftService(directory.path());
        QVERIFY(service->open(ownerA)); QTRY_VERIFY_WITH_TIMEOUT(service->available(), 2000);
        const auto lease = service->lease(); bool armed = false;
        connect(service, &NativeDraftService::stateChanged, this, [&] {
            if (armed && service) delete service;
        });
        QVERIFY(service->apply(lease, {record(messageId)}, {}));
        QVERIFY(service->apply(lease, {record(returnedId)}, {})); armed = true;
        QTRY_VERIFY_WITH_TIMEOUT(!service, 2000);
        NativeDraftStore reopened(directory.path()); QCOMPARE(reopened.open(ownerA), Failure::None);
        QCOMPARE(reopened.list().rows.size(), 2);
    }

    void deletedReceiverCancelsOnlyItsReceiptNotTheDurableWrite()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        auto *receiver = new QObject; int callbacks = 0;
        QVERIFY(service.apply(lease, {record()}, {}, receiver, [&](Failure) { ++callbacks; }));
        delete receiver;
        bool flushed = false; QVERIFY(service.flush(this, [&](Failure f) { flushed = f == Failure::None; }));
        QTRY_VERIFY_WITH_TIMEOUT(flushed, 2000); QCOMPARE(callbacks, 0);
        bool read = false; QVERIFY(service.read(lease, returnedId, this, [&](auto r) { read = r.failure == Failure::None && r.record.has_value(); }));
        QTRY_VERIFY_WITH_TIMEOUT(read, 2000);
    }

    void destructionFromShutdownFinishedDoesNotWaitOnAStoppedWorker()
    {
        QTemporaryDir directory; QPointer<NativeDraftService> service = new NativeDraftService(directory.path());
        QVERIFY(service->open(ownerA)); QTRY_VERIFY_WITH_TIMEOUT(service->available(), 2000);
        connect(service, &NativeDraftService::shutdownFinished, this, [service] { delete service; });
        service->shutdown(); QTRY_VERIFY_WITH_TIMEOUT(!service, 2000);
    }

    void shutdownHasAReservedBarrierBehindFullAcceptedQueues()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        WriterLock lock; QVERIFY(lock.acquire(directory.filePath(ownerA + ".sqlite3")));
        int writes = 0, barriers = 0;
        for (int i = 0; i < NativeDraftService::MaximumDataJobs; ++i) {
            auto r = record(QUuid::createUuid().toString(QUuid::WithoutBraces));
            QVERIFY(service.apply(lease, {r}, {}, this, [&](Failure f) { if (f == Failure::None) ++writes; }));
        }
        QVERIFY(service.flush(this, [&](Failure f) { if (f == Failure::None) ++barriers; }));
        QVERIFY(service.flush(this, [&](Failure f) { if (f == Failure::None) ++barriers; }));
        service.shutdown(); QVERIFY(!service.available()); lock.release();
        QTRY_VERIFY_WITH_TIMEOUT(service.stopped(), 3000);
        QCOMPARE(writes, NativeDraftService::MaximumDataJobs); QCOMPARE(barriers, 2);
        NativeDraftStore reopened(directory.path()); QCOMPARE(reopened.open(ownerA), Failure::None);
        QCOMPARE(reopened.list().rows.size(), NativeDraftService::MaximumDataJobs);
    }

    void invalidOwnerAndForeignServiceLeasesFailBeforeStorageAdmission()
    {
        QTemporaryDir directory; NativeDraftService first(directory.filePath("a")), second(directory.filePath("b"));
        QVERIFY(!first.open("bad")); QCOMPARE(first.failure(), Failure::InvalidOwner);
        QVERIFY(first.open(ownerA)); QVERIFY(second.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(first.available() && second.available(), 2000);
        bool rejected = false; QVERIFY(!second.apply(first.lease(), {record()}, {}, this, [&](Failure f) { rejected = f == Failure::Unavailable; }));
        QTRY_VERIFY_WITH_TIMEOUT(rejected, 2000);
        bool invalid = false; auto malformed = record(); malformed.text = ":::private";
        QVERIFY(!first.apply(first.lease(), {malformed}, {}, this, [&](Failure f) { invalid = f == Failure::InvalidRecord; }));
        QTRY_VERIFY_WITH_TIMEOUT(invalid, 2000);
        first.shutdown(); QTRY_VERIFY_WITH_TIMEOUT(first.stopped(), 2000);
        bool afterStop = false; QVERIFY(!first.apply(first.lease(), {record()}, {}, this, [&](Failure f) { afterStop = f == Failure::Unavailable; }));
        QTRY_VERIFY_WITH_TIMEOUT(afterStop, 2000);
    }

    void previewReadsAreBoundedAndFencedByTheForegroundOwner()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY(service.available()); const auto lease = service.lease(); bool saved = false;
        QVERIFY(service.apply(lease, {record()}, {}, this, [&](Failure f) { saved = f == Failure::None; })); QTRY_VERIFY(saved);
        bool loaded = false; NativeDraftStore::PreviewPage preview;
        QVERIFY(service.previews(lease, {}, this, [&](auto p) { preview = p; loaded = true; })); QTRY_VERIFY(loaded);
        QCOMPARE(preview.failure, Failure::None); QCOMPARE(preview.rows.size(), 1); QCOMPARE(preview.rows.first().text, record().text);
        bool settled = false; QVERIFY(service.previews(lease, {}, this, [&](auto p) { preview = p; settled = true; }));
        QVERIFY(service.open(ownerB)); QTRY_VERIFY(settled && service.available());
        QCOMPARE(preview.failure, Failure::Unavailable); QVERIFY(preview.rows.isEmpty());
        int invalid = 0; QVERIFY(!service.previews(service.lease(), QString(129, 'x'), this, [&](auto p) {
            if (p.failure == Failure::InvalidRecord) ++invalid;
        })); QTRY_COMPARE(invalid, 1);
    }

    void acceptedStreamRevocationKeepsItsOwnerAndDrainsOnDestruction()
    {
        QTemporaryDir directory;
        {
            NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA)); QTRY_VERIFY(service.available());
            const auto lease = service.lease(); bool saved = false; auto other = record(messageId); other.stream = secondTopicId;
            QVERIFY(service.apply(lease, {record(), other}, {}, this, [&](Failure f) { saved = f == Failure::None; })); QTRY_VERIFY(saved);
            QVERIFY(service.retainStreams(lease, {streamId})); QVERIFY(service.open(ownerB));
        }
        NativeDraftStore a(directory.path()); QCOMPARE(a.open(ownerA), Failure::None);
        QVERIFY(a.read(returnedId).record); QVERIFY(!a.read(messageId).record);
        NativeDraftStore b(directory.path()); QCOMPARE(b.open(ownerB), Failure::None); QVERIFY(b.list().rows.isEmpty());
    }

    void invalidReadAddressesNeverEnterTheBoundedWorkerQueue()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); QVERIFY(service.open(ownerA));
        QTRY_VERIFY_WITH_TIMEOUT(service.available(), 2000); const auto lease = service.lease();
        int invalid = 0;
        QVERIFY(!service.read(lease, QString(10000, 'x'), this, [&](auto r) { if (r.failure == Failure::InvalidRecord) ++invalid; }));
        QVERIFY(!service.latest(lease, "bad", topicId, this, [&](auto r) { if (r.failure == Failure::InvalidRecord) ++invalid; }));
        QVERIFY(!service.list(lease, QString(10000, 'x'), this, [&](auto r) { if (r.failure == Failure::InvalidRecord) ++invalid; }));
        QCOMPARE(service.pendingDataJobs(), 0); QCOMPARE(service.pendingBytes(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(invalid, 3, 2000);
    }
};

QTEST_GUILESS_MAIN(NativeDraftServiceTest)
#include "native-draft-service-test.moc"
