#include "native-cache-service.h"

#include <QJsonObject>
#include <QSemaphore>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <memory>
#include <QTimer>
#include <atomic>
#include <QTemporaryDir>
#include <QTest>

using namespace workspace;

namespace
{
const QUrl Server("https://cache-service.invalid");
const QString Project = "11111111-1111-4111-8111-111111111111";
const QString User = "22222222-2222-4222-8222-222222222222";
const QString Stream = "33333333-3333-4333-8333-333333333333";
const QString Topic = "44444444-4444-4444-8444-444444444444";
QJsonArray rows()
{
    return {
        QJsonObject{{"uuid", "88888888-8888-4888-8888-888888888888"},
                    {"project_id", Project},
                    {"stream_uuid", Stream},
                    {"topic_uuid", Topic},
                    {"author_uuid", User},
                    {"created_at", "2026-01-02T10:00:00.000001Z"},
                    {"read", false},
                    {"is_own", true},
                    {"payload", QJsonObject{{"kind", "markdown"}, {"content", QString(128 * 1024, 'x')}}}}};
}
} // namespace

class NativeCacheServiceTest final : public QObject
{
    Q_OBJECT
  private slots:
    // Same five logical local-worker oracles; callbacks own their completion
    // state so even an expected capability RED can unwind without dangling refs.
    void messageBodyRunsOnExistingWorkerAndReturnsOnlyToCurrentLease()
    {
        struct State { bool saved=false, returned=false, rejected=false; std::optional<NativeCacheService::MessageBody> body; };
        const auto state=std::make_shared<State>();
        QTemporaryDir directory; std::atomic_bool workerSeen{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            workerSeen = QThread::currentThread() != QCoreApplication::instance()->thread(); return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available());
        const auto lease = service.lease(); auto row = rows().first().toObject(); row["user_uuid"] = User;
        const auto id = row.value("uuid").toString();
        QVERIFY(service.putMessageBody(lease, row, this, [state](bool okay) { state->saved=okay; })); QTRY_VERIFY(state->saved);
        service.getMessageBody(lease, id, this, [this,state](auto value) {
            QCOMPARE(QThread::currentThread(), thread()); state->body=value; state->returned=true;
        });
        QVERIFY(!state->returned); QTRY_VERIFY(state->returned); QVERIFY(state->body); QCOMPARE(state->body->row, row); QVERIFY(workerSeen.load());
        auto foreign = row; foreign["user_uuid"] = Project;
        QVERIFY(!service.putMessageBody(lease, foreign, this, [state](bool okay) { state->rejected=!okay; })); QTRY_VERIFY(state->rejected);
        service.close(); state->returned=false; state->body.reset();
        service.getMessageBody(lease, id, this, [state](auto value) { state->body=value; state->returned=true; });
        QTRY_VERIFY(state->returned); QVERIFY(!state->body);
    }

    void authoritativeBodyOmissionSurvivesFullOrdinaryQueuesAndDominatesOldWrites()
    {
        struct State {
            bool seeded=false, flushed=false, oldRead=false, omitted=false, loaded=false;
            int dropped=0, controls=0; std::optional<NativeCacheService::MessageBody> stale, body;
        };
        const auto state=std::make_shared<State>();
        QTemporaryDir directory; QSemaphore release; std::atomic_bool block{false}, entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            if (block.exchange(false)) { entered = true; release.acquire(); } return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available()); const auto lease = service.lease();
        auto row = rows().first().toObject(); row["user_uuid"] = User; const auto id = row.value("uuid").toString();
        const NativeCache::Key history{NativeCache::Kind::History, Stream, Topic, {}};
        const NativeCache::Key aggregate{NativeCache::Kind::History, Stream, {}, {}};
        QVERIFY(service.putMessageBody(lease, row, this, [state](bool okay) { state->seeded=okay; })); QTRY_VERIFY(state->seeded);
        state->seeded=false; QVERIFY(service.put(lease, history, {row}, {}, this, [state](bool okay) { state->seeded=okay; })); QTRY_VERIFY(state->seeded);
        service.flush(this, [state](bool okay) { state->flushed=okay; }); QTRY_VERIFY(state->flushed);
        auto unblock = qScopeGuard([&] { release.release(); }); block = true;
        service.getMessageBody(lease, id, this, [state](auto value) { state->stale=value; state->oldRead=true; }); QTRY_VERIFY(entered.load());
        QVERIFY(service.putMessageBody(lease, row, this, [state](bool okay) { if (!okay) ++state->dropped; }));
        QVERIFY(service.put(lease, aggregate, {row}, {}, this, [state](bool okay) { if (!okay) ++state->dropped; }));
        QVERIFY(service.projectMessage(lease, row, NativeCache::Projection::Payload, this, [state](bool okay) { if (!okay) ++state->dropped; }));
        QStringList otherIds;
        for (int n = 0; n < 4; ++n) {
            auto other = row; other["uuid"] = QUuid::createUuid().toString(QUuid::WithoutBraces); otherIds.append(other.value("uuid").toString());
            QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, QByteArray::number(n)}, {other}));
        }
        for (int n = 0; n < 64; ++n)
            QVERIFY(service.invalidate(lease, NativeCache::Kind::History, User, Project, this, [state](bool okay) { if (okay) ++state->controls; }));
        QVERIFY(service.removeMessageBodyAndHistory(lease, id, this, [state](bool okay) { state->omitted=okay; }));
        QTRY_COMPARE(state->dropped, 3);
        release.release(); unblock.dismiss(); QTRY_VERIFY(state->omitted); QTRY_COMPARE(state->controls, 64); QTRY_VERIFY(state->oldRead); QVERIFY(!state->stale);
        state->flushed=false; service.flush(this, [state](bool okay) { state->flushed=okay; }); QTRY_VERIFY(state->flushed);
        service.getMessageBody(lease, id, this, [state](auto value) { state->body=value; state->loaded=true; }); QTRY_VERIFY(state->loaded); QVERIFY(!state->body);
        for (const auto &other : otherIds) {
            state->loaded=false; state->body.reset(); service.getMessageBody(lease, other, this, [state](auto value) { state->body=value; state->loaded=true; });
            QTRY_VERIFY(state->loaded); QVERIFY(state->body);
        }
    }

    void requiredBodyOmissionRetainsCapturedOwnerAcrossLifecycle_data()
    {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"close", "switch", "shutdown"}) QTest::newRow(phase) << QString(phase);
    }
    void requiredBodyOmissionRetainsCapturedOwnerAcrossLifecycle()
    {
        struct State { bool seeded=false, erased=false, flushed=false; }; const auto state=std::make_shared<State>();
        QFETCH(QString, phase); QTemporaryDir directory; QSemaphore release; std::atomic_bool block{false}, entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            if (block.exchange(false)) { entered = true; release.acquire(); } return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available()); const auto lease = service.lease();
        auto row = rows().first().toObject(); row["user_uuid"] = User; const auto id = row.value("uuid").toString();
        QVERIFY(service.putMessageBody(lease, row, this, [state](bool okay) { state->seeded=okay; })); QTRY_VERIFY(state->seeded);
        auto unblock = qScopeGuard([&] { release.release(); }); block = true;
        service.getMessageBody(lease, id, this, [](auto) {}); QTRY_VERIFY(entered.load());
        QVERIFY(service.removeMessageBodyAndHistory(lease, id, this, [state](bool okay) { state->erased=okay; }));
        if (phase == "close") service.close();
        else if (phase == "switch") QVERIFY(service.open(Server, Project, Project));
        else service.shutdown();
        release.release(); unblock.dismiss(); QTRY_VERIFY(state->erased);
        if (phase == "shutdown") QTRY_VERIFY(service.stopped());
        else { service.flush(this, [state](bool okay) { state->flushed=okay; }); QTRY_VERIFY(state->flushed); }
        NativeCache inspect(directory.path()); QVERIFY(inspect.open(Server, Project, User));
        QVERIFY(!inspect.getMessageBody(inspect.lease(), id));
    }

    void init()
    {
        QTest::failOnWarning();
    }
    void rejectedRetainedWritesKeepTheNewestGlobalBarrierAfterQueuedSnapshots()
    {
        QTemporaryDir directory; QSemaphore release;
        std::atomic_bool block{false}, entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            if (block.exchange(false)) { entered = true; release.acquire(); }
            return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available());
        const auto lease=service.lease(); const NativeCache::Key catalog{NativeCache::Kind::Streams,{},{},{}};
        QJsonObject row{{"uuid",Stream},{"project_id",Project},{"name","Seed"},{"encryption",false}};
        bool seeded=false; QVERIFY(service.put(lease,catalog,{row},{},this,[&](bool okay){seeded=okay;})); QTRY_VERIFY(seeded);
        auto unblock=qScopeGuard([&]{release.release();});block=true;
        service.get(lease,catalog,this,[](auto){});QTRY_VERIFY(entered.load());
        int dropped=0;
        for(int n=0;n<NativeCacheService::MaximumQueuedJobs-1;++n)
            QVERIFY(service.put(lease,{NativeCache::Kind::History,Stream,Topic,QByteArray::number(n)},rows(),{},this,[&](bool){++dropped;}));
        int cleaned=0;
        for(int n=0;n<64;++n)QVERIFY(service.invalidate(lease,NativeCache::Kind::History,User,Project,this,[&](bool okay){if(okay)++cleaned;}));
        row["name"]="First local projection";QVERIFY(!service.put(lease,catalog,{row},{},nullptr,{},true));
        // Existing invalidation cancels these queued optional pages before its
        // own full-control admission fails. This opens real optional capacity.
        QVERIFY(!service.invalidate(lease,NativeCache::Kind::History,Stream,Topic,this,[](bool){}));
        QTRY_COMPARE(dropped,NativeCacheService::MaximumQueuedJobs-1);
        row["name"]="Fetched between local projections";QVERIFY(service.put(lease,catalog,{row}));
        int refilled=0;
        for(int n=0;n<NativeCacheService::MaximumQueuedJobs-2;++n)
            QVERIFY(service.put(lease,{NativeCache::Kind::History,Stream,Topic,QByteArray("refill-")+QByteArray::number(n)},rows(),{},this,[&](bool){++refilled;}));
        // A second retained route must not coalesce away the ordinary catalog
        // snapshot: the test needs a genuine rejection at full optional capacity.
        const NativeCache::Key users{NativeCache::Kind::Users,{},{},{}};
        QVERIFY(!service.put(lease,users,{QJsonObject{{"uuid",User},{"username","second-local-projection"}}},{},nullptr,{},true));
        QVERIFY(!service.markStale(lease,NativeCache::Kind::Streams));
        release.release();unblock.dismiss();QTRY_COMPARE(cleaned,64);
        QTRY_COMPARE(refilled,NativeCacheService::MaximumQueuedJobs-2);
        bool flushed=false;service.flush(this,[&](bool okay){flushed=okay;});QTRY_VERIFY(flushed);
        bool loaded=false;std::optional<NativeCache::Entry> saved;
        service.get(lease,catalog,this,[&](auto value){saved=value;loaded=true;});QTRY_VERIFY(loaded);
        QVERIFY(saved);QCOMPARE(saved->rows.first().toObject()["name"].toString(),QString("Fetched between local projections"));
        QVERIFY2(!saved->fresh,"A coalesced global barrier ran before a superseded queued snapshot");
    }

    void retainedPutIsSavedStaleAndAuthoritativePutBecomesFresh()
    {
        QTemporaryDir directory; NativeCacheService service(directory.path());
        QVERIFY(service.open(Server,Project,User)); QTRY_VERIFY(service.available());
        const auto lease=service.lease(); const NativeCache::Key key{NativeCache::Kind::Streams,{},{},{}};
        const QJsonArray projection{QJsonObject{{"uuid",Stream},{"project_id",Project},{"name","Projection"},{"encryption",false}}};
        bool done=false,okay=false;
        QVERIFY(service.put(lease,key,projection,{},this,[&](bool result){okay=result;done=true;},true));
        QTRY_VERIFY(done); QVERIFY(okay);
        std::optional<NativeCache::Entry> saved;bool loaded=false;
        service.get(lease,key,this,[&](auto result){saved=result;loaded=true;});QTRY_VERIFY(loaded);
        QVERIFY(saved);QVERIFY(!saved->fresh);QCOMPARE(saved->rows,projection);
        done=false;okay=false;
        QVERIFY(service.put(lease,key,projection,{},this,[&](bool result){okay=result;done=true;}));
        QTRY_VERIFY(done);QVERIFY(okay);loaded=false;saved.reset();
        service.get(lease,key,this,[&](auto result){saved=result;loaded=true;});QTRY_VERIFY(loaded);
        QVERIFY(saved);QVERIFY(saved->fresh);QCOMPARE(saved->rows,projection);
    }
    void retainedPutCoalescingPreservesTheLastPayloadAndFreshness_data()
    {
        QTest::addColumn<bool>("lastRetained");QTest::newRow("retained-last")<<true;QTest::newRow("authoritative-last")<<false;
    }
    void retainedPutCoalescingPreservesTheLastPayloadAndFreshness()
    {
        QFETCH(bool,lastRetained);QTemporaryDir directory;QSemaphore release;std::atomic<bool> block=false,entered=false;
        NativeCacheService service(directory.path(),{},false,[&]{if(block.exchange(false)){entered=true;release.acquire();}return qint64(10000);});
        QVERIFY(service.open(Server,Project,User));QTRY_VERIFY(service.available());const auto lease=service.lease();
        const NativeCache::Key key{NativeCache::Kind::Streams,{},{},{}};
        auto row=QJsonObject{{"uuid",Stream},{"project_id",Project},{"name","Seed"},{"encryption",false}};
        bool seeded=false;QVERIFY(service.put(lease,key,{row},{},this,[&](bool result){seeded=result;}));QTRY_VERIFY(seeded);
        auto unblock=qScopeGuard([&]{release.release();});block=true;service.get(lease,key,this,[](auto){});QTRY_VERIFY(entered.load());
        bool firstDone=false,firstOkay=true,lastDone=false,lastOkay=false;
        row["name"]="Earlier";QVERIFY(service.put(lease,key,{row},{},this,[&](bool result){firstDone=true;firstOkay=result;},!lastRetained));
        row["name"]="Latest";QVERIFY(service.put(lease,key,{row},{},this,[&](bool result){lastDone=true;lastOkay=result;},lastRetained));
        QTRY_VERIFY(firstDone);QVERIFY(!firstOkay);
        release.release();unblock.dismiss();QTRY_VERIFY(lastDone);QVERIFY(lastOkay);
        std::optional<NativeCache::Entry> saved;bool loaded=false;
        service.get(lease,key,this,[&](auto result){saved=result;loaded=true;});QTRY_VERIFY(loaded);
        QVERIFY(saved);QCOMPARE(saved->rows.first().toObject()["name"].toString(),QString("Latest"));QCOMPARE(saved->fresh,!lastRetained);
    }

    void topicDeletionRevokesQueuedAggregateRefillAndRead()
    {
        QTemporaryDir directory; QSemaphore gate; std::atomic_bool hold{false}, entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); } return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available()); const auto lease = service.lease();
        const NativeCache::Key aggregate{NativeCache::Kind::History, Stream, {}, {}};
        bool seeded = false; service.put(lease, aggregate, rows(), {}, this, [&](bool ok) { seeded = ok; }); QTRY_VERIFY(seeded);
        hold = true; service.get(lease, aggregate, this, [](auto) {}); QTRY_VERIFY(entered.load());
        bool refillDone = false, refillOkay = true, readDone = false;
        service.put(lease, aggregate, rows(), {}, this, [&](bool ok) { refillDone = true; refillOkay = ok; });
        service.get(lease, aggregate, this, [&](auto saved) { readDone = true; QVERIFY(!saved); });
        QVERIFY(service.invalidate(lease, NativeCache::Kind::History, Stream, Topic));
        QTRY_VERIFY(refillDone); QVERIFY(!refillOkay); QTRY_VERIFY(readDone);
        gate.release(); bool flushed = false; service.flush(this, [&](bool ok) { flushed = ok; }); QTRY_VERIFY(flushed);
        std::optional<NativeCache::Entry> saved; bool loaded = false;
        service.get(lease, aggregate, this, [&](auto value) { saved = value; loaded = true; }); QTRY_VERIFY(loaded); QVERIFY(!saved);
    }

    void queuedProjectionPreservesIndependentMasksAndUnrelatedRoutes()
    {
        QTemporaryDir directory;
        QSemaphore gate; std::atomic_bool hold{false}, entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available());
        const auto lease = service.lease();
        const NativeCache::Key key{NativeCache::Kind::History, Stream, Topic, {}};
        auto original = rows().first().toObject(); original["starred"] = false;
        bool seeded = false;
        QVERIFY(service.put(lease, key, {original}, {}, this, [&](bool ok) { seeded = ok; })); QTRY_VERIFY(seeded);
        hold = true;
        service.get(lease, key, this, [](auto) {}); QTRY_VERIFY(entered.load());
        auto changed = original; changed["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "New content"}};
        QVERIFY(service.projectMessage(lease, changed, NativeCache::Projection::Payload));
        changed["starred"] = true;
        QVERIFY(service.projectMessage(lease, changed, NativeCache::Projection::Star));
        // A different topic deletion must not drop these admitted projections.
        QVERIFY(service.invalidate(lease, NativeCache::Kind::History, Stream, User));
        gate.release();
        bool flushed = false; service.flush(this, [&](bool ok) { flushed = ok; }); QTRY_VERIFY(flushed);
        std::optional<NativeCache::Entry> saved; bool loaded = false;
        service.get(lease, key, this, [&](auto value) { saved = value; loaded = true; }); QTRY_VERIFY(loaded);
        QVERIFY(saved && !saved->fresh); const auto row = saved->rows.first().toObject();
        QCOMPARE(row.value("payload"), changed.value("payload")); QVERIFY(row.value("starred").toBool());
    }

    void hardDeletionDominatesQueuedProjectionAndRefill()
    {
        QTemporaryDir directory;
        QSemaphore gate; std::atomic_bool hold{false}, entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available());
        const auto lease = service.lease(); const NativeCache::Key key{NativeCache::Kind::History, Stream, Topic, {}};
        bool seeded = false; QVERIFY(service.put(lease, key, rows(), {}, this, [&](bool ok) { seeded = ok; })); QTRY_VERIFY(seeded);
        hold = true; service.get(lease, key, this, [](auto) {}); QTRY_VERIFY(entered.load());
        QVERIFY(service.projectMessage(lease, rows().first().toObject(), NativeCache::Projection::Insert));
        QVERIFY(service.put(lease, key, rows()));
        QVERIFY(service.invalidate(lease, NativeCache::Kind::History, Stream, Topic));
        gate.release();
        bool flushed = false; service.flush(this, [&](bool ok) { flushed = ok; }); QTRY_VERIFY(flushed);
        std::optional<NativeCache::Entry> saved; bool loaded = false;
        service.get(lease, key, this, [&](auto value) { saved = value; loaded = true; }); QTRY_VERIFY(loaded);
        QVERIFY(!saved);
    }

    void queuedProjectionObeysOwnerCleanupAndShutdown_data()
    {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"close", "switch", "forget", "shutdown"}) QTest::newRow(phase) << QString::fromLatin1(phase);
    }
    void queuedProjectionObeysOwnerCleanupAndShutdown()
    {
        QFETCH(QString, phase);
        QTemporaryDir directory; QSemaphore gate; std::atomic_bool hold{false}, entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return qint64(10000);
        });
        QVERIFY(service.open(Server, Project, User)); QTRY_VERIFY(service.available());
        const auto lease = service.lease(); const NativeCache::Key key{NativeCache::Kind::History, Stream, Topic, {}};
        bool seeded = false; QVERIFY(service.put(lease, key, rows(), {}, this, [&](bool ok) { seeded = ok; })); QTRY_VERIFY(seeded);
        hold = true; service.get(lease, key, this, [](auto) {}); QTRY_VERIFY(entered.load());
        auto changed = rows().first().toObject(); changed["starred"] = true;
        QVERIFY(service.projectMessage(lease, changed, NativeCache::Projection::Star));
        if (phase == "forget") QVERIFY(service.clearOwner(lease));
        if (phase == "close") service.close();
        if (phase == "switch") QVERIFY(service.open(Server, Project, Topic));
        if (phase == "shutdown") service.shutdown();
        gate.release();
        if (phase != "shutdown") {
            bool flushed = false; service.flush(this, [&](bool okay) { flushed = okay; }); QTRY_VERIFY(flushed);
            service.shutdown();
        }
        QTRY_VERIFY(service.stopped());
        NativeCache check(directory.path()); QVERIFY(check.open(Server, Project, User));
        const auto saved = check.get(check.lease(), key);
        if (phase == "forget") QVERIFY(!saved);
        else { QVERIFY(saved && !saved->fresh); QCOMPARE(saved->rows.first().toObject().value("starred").toBool(), phase == "shutdown"); }
    }

    void stateObserversMayDestroyTheService_data()
    {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"opening", "ready", "applied", "clear-error", "closed-clear-error",
                                  "invalid-limit", "failed-open", "forget", "close", "shutdown", "finished"})
            QTest::newRow(phase) << QString::fromLatin1(phase);
    }
    void stateObserversMayDestroyTheService()
    {
        QFETCH(QString, phase);
        QTemporaryDir directory;
        QString cacheDirectory = directory.path();
        if (phase == "failed-open") {
            cacheDirectory = directory.filePath("not-a-directory");
            QFile blocker(cacheDirectory);
            QVERIFY(blocker.open(QIODevice::WriteOnly));
        }
        QPointer<NativeCacheService> service = new NativeCacheService(cacheDirectory);
        if (phase != "opening" && phase != "failed-open" && phase != "closed-clear-error" &&
            phase != "invalid-limit") {
            QVERIFY(service->open(Server, Project, User));
            QTRY_VERIFY(service->available());
        }
        if (phase == "forget") {
            bool written = false;
            QVERIFY(service->put(service->lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows(),
                                 {}, this, [&](bool okay) { written = okay; }));
            QTRY_VERIFY(written);
        }
        if (phase == "clear-error" || phase == "closed-clear-error")
            QVERIFY(!service->setDiskLimitBytes(0));
        const auto destroy = [&] { delete service.data(); };
        if (phase == "applied")
            connect(service, &NativeCacheService::limitApplied, this, destroy);
        else if (phase == "clear-error" || phase == "closed-clear-error" || phase == "invalid-limit" ||
                 phase == "failed-open")
            connect(service, &NativeCacheService::errorChanged, this, destroy);
        else if (phase == "finished")
            connect(service, &NativeCacheService::shutdownFinished, this, destroy);
        else
            connect(service, &NativeCacheService::stateChanged, this, [&] {
                if (service && (phase != "ready" || service->available()))
                    destroy();
            });
        if (phase == "opening" || phase == "failed-open")
            service->open(Server, Project, User);
        else if (phase == "forget")
            service->clearOwner(service->lease());
        else if (phase == "shutdown" || phase == "finished")
            service->shutdown();
        else if (phase == "close")
            service->close();
        else if (phase == "invalid-limit")
            service->setDiskLimitBytes(0);
        else
            service->setDiskLimitBytes(NativeCache::DefaultDiskLimitBytes);
        QTRY_VERIFY(!service);
        if (phase == "forget") {
            NativeCache check(directory.path());
            QVERIFY(check.open(Server, Project, User));
            QVERIFY(!check.get(check.lease(), {NativeCache::Kind::History, Stream, Topic, {}}));
        }
    }

    void completionMayDestroyTheService_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"open", "get", "put", "project", "forget", "flush"})
            QTest::newRow(operation) << QString::fromLatin1(operation);
    }
    void completionMayDestroyTheService()
    {
        QFETCH(QString, operation);
        QTemporaryDir directory;
        QPointer<NativeCacheService> service = new NativeCacheService(directory.path());
        const auto destroy = [&](bool okay) {
            QVERIFY(okay);
            delete service.data();
        };
        if (operation == "open")
            QVERIFY(service->open(Server, Project, User, this, destroy));
        else {
            QVERIFY(service->open(Server, Project, User));
            QTRY_VERIFY(service->available());
            bool written = false;
            QVERIFY(service->put(service->lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows(),
                                 {}, this, [&](bool okay) { written = okay; }));
            QTRY_VERIFY(written);
            if (operation == "get")
                service->get(service->lease(), {NativeCache::Kind::History, Stream, Topic, {}}, this,
                             [&](auto entry) {
                                 QVERIFY(entry);
                                 delete service.data();
                             });
            else if (operation == "put")
                QVERIFY(service->put(service->lease(), {NativeCache::Kind::History, Stream, Topic, {}},
                                     rows(), {}, this, destroy));
            else if (operation == "project") {
                auto canonical = rows().first().toObject(); canonical["starred"] = true;
                QVERIFY(service->projectMessage(service->lease(), canonical, NativeCache::Projection::Star, this, destroy));
            } else if (operation == "forget")
                QVERIFY(service->clearOwner(service->lease(), this, destroy));
            else
                service->flush(this, destroy);
        }
        QTRY_VERIFY(!service);
    }

    void workerPersistsPagesAndDeliversOnlyOnTheReceiverThread()
    {
        QTemporaryDir directory;
        NativeCacheService service(directory.path());
        bool ready = false;
        QVERIFY(service.open(Server, Project, User, this, [&](bool okay) { ready = okay; }));
        QVERIFY(!service.available());
        QTRY_VERIFY(ready);
        QVERIFY(service.available());
        bool written = false;
        const auto lease = service.lease();
        QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, {}}, rows(), "next", this,
                            [&](bool okay) { written = okay; }));
        QTRY_VERIFY(written);
        bool received = false;
        service.get(lease, {NativeCache::Kind::History, Stream, Topic, {}}, this, [&](auto entry) {
            QCOMPARE(QThread::currentThread(), thread());
            QVERIFY(entry);
            QCOMPARE(entry->rows, rows());
            QCOMPARE(entry->nextMarker, QByteArray("next"));
            QVERIFY(entry->fresh);
            received = true;
        });
        QTRY_VERIFY(received);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
        NativeCache reopened(directory.path());
        QVERIFY(reopened.open(Server, Project, User));
        const auto entry = reopened.get(reopened.lease(), {NativeCache::Kind::History, Stream, Topic, {}});
        QVERIFY(entry && !entry->fresh);
    }

    void resizeDoesNotBlockAGuiTimerThatReleasesTheRealSqliteWriter()
    {
        QTemporaryDir directory;
        {
            NativeCache store(directory.path());
            QVERIFY(store.open(Server, Project, User));
            for (int i = 0; i < 32; ++i)
                QVERIFY(store.put(store.lease(),
                                  {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)},
                                  rows()));
        }
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto applied = service.appliedDiskLimitBytes();
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(directory.filePath("cache.sqlite3"));
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("BEGIN IMMEDIATE"));
            bool timerRan = false, miss = false;
            QVERIFY(service.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
            QVERIFY(!service.available());
            QVERIFY(service.maintenanceBusy());
            QCOMPARE(service.requestedDiskLimitBytes(), NativeCache::MinimumDiskLimitBytes);
            QCOMPARE(service.appliedDiskLimitBytes(), applied);
            service.get(service.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, this,
                        [&](auto entry) {
                            QVERIFY(!entry);
                            miss = true;
                        });
            QTimer::singleShot(0, this, [&] {
                QVERIFY(query.exec("ROLLBACK"));
                timerRan = true;
            });
            QTRY_VERIFY(timerRan && miss);
            QTRY_VERIFY(!service.maintenanceBusy());
            QVERIFY(service.available());
            QCOMPARE(service.appliedDiskLimitBytes(), NativeCache::MinimumDiskLimitBytes);
            QVERIFY(service.diskUsageBytes() <= NativeCache::MinimumDiskLimitBytes);
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void queuedSnapshotsRemainAheadOfTheirConfirmedReadPatch()
    {
        QTemporaryDir directory;
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto lease = service.lease();
        QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        QVERIFY(service.markHistoryRead(lease, Stream, Topic, {"88888888-8888-4888-8888-888888888888"}));
        bool drained = false;
        service.flush(this, [&](bool okay) { drained = okay; });
        QTRY_VERIFY(drained);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
        NativeCache reopened(directory.path());
        QVERIFY(reopened.open(Server, Project, User));
        auto entry = reopened.get(reopened.lease(), {NativeCache::Kind::History, Stream, Topic, {}});
        QVERIFY(entry);
        QVERIFY(entry->rows.first().toObject().value("read").toBool());
    }

    void writeAdmissionIsBoundedAndSameKeyUpdatesCoalesce()
    {
        QTemporaryDir directory;
        QSemaphore gate;
        std::atomic_bool entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            entered = true;
            gate.tryAcquire(1, 2000);
            return 1000;
        });
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto lease = service.lease();
        QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, "active"}, rows()));
        QTRY_VERIFY(entered.load());
        for (int i = 0; i < NativeCacheService::MaximumQueuedJobs - 1; ++i)
            QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)},
                                rows()));
        QVERIFY(!service.put(lease, {NativeCache::Kind::History, Stream, Topic, "overflow"}, rows()));
        QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, "0"}, rows()));
        gate.release(NativeCacheService::MaximumQueuedJobs);
        bool drained = false;
        service.flush(this, [&](bool okay) { drained = okay; });
        QTRY_VERIFY(drained);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void forgottenOwnerCannotBeRecreatedByALateAcceptedPut()
    {
        QTemporaryDir directory;
        QSemaphore gate;
        std::atomic_bool entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            entered = true;
            gate.tryAcquire(1, 2000);
            return 1000;
        });
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto old = service.lease();
        QVERIFY(service.put(old, {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        QTRY_VERIFY(entered.load());
        bool erased = false;
        QVERIFY(service.clearOwner(old, this, [&](bool okay) { erased = okay; }));
        QVERIFY(!service.put(old, {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        gate.release(2);
        QTRY_VERIFY(erased);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
        NativeCache reopened(directory.path());
        QVERIFY(reopened.open(Server, Project, User));
        QVERIFY(!reopened.get(reopened.lease(), {NativeCache::Kind::History, Stream, Topic, {}}));
    }

    void destroyedReceiverNeverReceivesAPendingRead()
    {
        QTemporaryDir directory;
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        bool written = false;
        QVERIFY(service.put(service.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows(), {},
                            this, [&](bool okay) { written = okay; }));
        QTRY_VERIFY(written);
        bool called = false;
        auto *receiver = new QObject;
        service.get(service.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, receiver,
                    [&](auto) { called = true; });
        delete receiver;
        bool drained = false;
        service.flush(this, [&](bool okay) { drained = okay; });
        QTRY_VERIFY(drained);
        QVERIFY(!called);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void requiredInvalidationsCannotOverflowBehindAnOptionalWrite()
    {
        QTemporaryDir directory;
        QSemaphore gate;
        std::atomic_bool entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            entered = true;
            gate.tryAcquire(1, 2000);
            return 1000;
        });
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto lease = service.lease();
        QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        QTRY_VERIFY(entered.load());
        for (int i = 0; i < 200; ++i)
            QVERIFY(
                service.invalidate(lease, NativeCache::Kind::History, Stream,
                                   QString("77777777-7777-4777-8777-%1").arg(i, 12, 16, QLatin1Char('0'))));
        QVERIFY(service.invalidate(lease, NativeCache::Kind::History, Stream, Topic));
        gate.release();
        bool drained = false;
        service.flush(this, [&](bool okay) { drained = okay; });
        QTRY_VERIFY(drained);
        bool missed = false;
        service.get(lease, {NativeCache::Kind::History, Stream, Topic, {}}, this, [&](auto entry) {
            QVERIFY(!entry);
            missed = true;
        });
        QTRY_VERIFY(missed);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void privacyCleanupHasReservedAdmissionBehindFullRequiredReadQueue()
    {
        QTemporaryDir directory;
        {
            NativeCache seed(directory.path());
            QVERIFY(seed.open(Server, Project, User));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        }
        QSemaphore gate;
        std::atomic_bool entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            entered = true;
            gate.tryAcquire(1, 2000);
            return 1000;
        });
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto old = service.lease();
        QVERIFY(service.put(old, {NativeCache::Kind::History, Stream, Topic, "blocked"}, rows()));
        QTRY_VERIFY(entered.load());
        for (int i = 0; i < 64; ++i)
            QVERIFY(service.markHistoryRead(old, Stream, Topic, {"88888888-8888-4888-8888-888888888888"},
                                            this, [](bool) {}));
        bool erased = false;
        const bool admitted = service.clearOwner(old, this, [&](bool okay) { erased = okay; });
        gate.release(2);
        QVERIFY(admitted);
        QTRY_VERIFY(erased);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
        NativeCache check(directory.path());
        QVERIFY(check.open(Server, Project, User));
        QVERIFY(!check.get(check.lease(), {NativeCache::Kind::History, Stream, Topic, {}}));
    }

    void queuedRawJsonBytesHaveAnIndependentBound()
    {
        QTemporaryDir directory;
        QSemaphore gate;
        std::atomic_bool entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            entered = true;
            gate.tryAcquire(1, 2000);
            return 1000;
        });
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto lease = service.lease();
        QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, "blocked"}, rows()));
        QTRY_VERIFY(entered.load());
        auto value = rows().first().toObject();
        value["payload"] = QJsonObject{{"kind", "markdown"}, {"content", QString(7 * 1024 * 1024, 'x')}};
        for (int i = 0; i < 4; ++i)
            QVERIFY(service.put(lease, {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)},
                                QJsonArray{value}));
        QVERIFY(!service.put(lease, {NativeCache::Kind::History, Stream, Topic, "over-byte-budget"},
                             QJsonArray{value}));
        gate.release(8);
        bool drained = false;
        service.flush(this, [&](bool okay) { drained = okay; });
        QTRY_VERIFY(drained);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void callbacksNeverRunOnAForeignReceiverThread()
    {
        QTemporaryDir directory;
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        QThread foreign;
        auto *receiver = new QObject;
        receiver->moveToThread(&foreign);
        connect(&foreign, &QThread::finished, receiver, &QObject::deleteLater);
        foreign.start();
        std::atomic_bool called{false};
        service.get(service.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, receiver,
                    [&](auto) { called = true; });
        bool drained = false;
        service.flush(this, [&](bool okay) { drained = okay; });
        QTRY_VERIFY(drained);
        foreign.quit();
        QVERIFY(foreign.wait(2000));
        QVERIFY(!called.load());
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void aSupersededOwnerNeverReceivesAStaleSuccessfulRead()
    {
        QTemporaryDir directory;
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        bool written = false;
        QVERIFY(service.put(service.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows(), {},
                            this, [&](bool okay) { written = okay; }));
        QTRY_VERIFY(written);
        const auto old = service.lease();
        bool delivered = false;
        service.get(old, {NativeCache::Kind::History, Stream, Topic, {}}, this, [&](auto entry) {
            QVERIFY(!entry);
            delivered = true;
        });
        QVERIFY(service.open(Server, Project, "99999999-9999-4999-8999-999999999999"));
        QTRY_VERIFY(service.available());
        QTRY_VERIFY(delivered);
        QVERIFY(service.lease().generation != old.generation);
        QVERIFY(service.lease().owner != old.owner);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void shutdownCancelsMaintenanceAndDrainsAcceptedSnapshotWrites()
    {
        QTemporaryDir directory;
        QSemaphore gate;
        std::atomic_bool entered{false};
        NativeCacheService service(directory.path(), {}, false, [&] {
            entered = true;
            gate.tryAcquire(1, 2000);
            return 1000;
        });
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        QVERIFY(service.put(service.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        QTRY_VERIFY(entered.load());
        service.shutdown();
        QVERIFY(!service.stopped());
        QVERIFY(!service.available());
        bool guiReleased = false;
        QTimer::singleShot(0, this, [&] {
            guiReleased = true;
            gate.release(3);
        });
        QTRY_VERIFY(service.stopped());
        QVERIFY(guiReleased);
        NativeCache check(directory.path());
        QVERIFY(check.open(Server, Project, User));
        QVERIFY(check.get(check.lease(), {NativeCache::Kind::History, Stream, Topic, {}}));
    }

    void storageCancellationStopsAnEvictionThatAlreadyDeletedRows()
    {
        QTemporaryDir directory;
        NativeCache store(directory.path());
        QVERIFY(store.open(Server, Project, User));
        for (int i = 0; i < 32; ++i)
            QVERIFY(store.put(store.lease(),
                              {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, rows()));
        int polls = 0;
        store.setCancellationCheck([&] { return ++polls >= 12; });
        QVERIFY(!store.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
        QVERIFY(!store.available());
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(directory.filePath("cache.sqlite3"));
            QVERIFY(database.open());
            {
                QSqlQuery query(database);
                QVERIFY(query.exec("SELECT COUNT(*) FROM entries"));
                QVERIFY(query.next());
                QVERIFY(query.value(0).toInt() > 0);
                QVERIFY(query.value(0).toInt() < 32);
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        store.setCancellationCheck({});
        QVERIFY(store.open(Server, Project, User));
        QVERIFY(store.diskUsageBytes() <= NativeCache::MinimumDiskLimitBytes);
    }

    void completedWritesPublishTheirCoherentUsageSnapshot()
    {
        QTemporaryDir directory;
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        QSignalSpy changed(&service, &NativeCacheService::stateChanged);
        bool written = false;
        QVERIFY(service.put(service.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows(), {},
                            this, [&](bool okay) { written = okay; }));
        QTRY_VERIFY(written);
        QVERIFY(service.diskUsageBytes() > 0);
        QVERIFY(!changed.empty());
        QVERIFY(!service.maintenanceBusy());
        QCOMPARE(service.appliedDiskLimitBytes(), service.requestedDiskLimitBytes());
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void clearingALimitErrorPublishesOnlyACoherentPendingResize()
    {
        QTemporaryDir directory;
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        QVERIFY(!service.setDiskLimitBytes(0));
        QVERIFY(!service.error().isEmpty());
        bool observed = false;
        connect(&service, &NativeCacheService::errorChanged, this, [&] {
            if (service.error().isEmpty()) {
                observed = true;
                QVERIFY(service.maintenanceBusy());
                QVERIFY(!service.available());
                QCOMPARE(service.requestedDiskLimitBytes(), NativeCache::MinimumDiskLimitBytes);
            }
        });
        QVERIFY(service.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
        QVERIFY(observed);
        QTRY_VERIFY(!service.maintenanceBusy());
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void foreignCleanupFailureAlwaysReportsASafeError()
    {
        QTemporaryDir directory;
        {
            NativeCache seed(directory.path());
            QVERIFY(seed.open(Server, Project, "99999999-9999-4999-8999-999999999999"));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        }
        NativeCacheService service(directory.path());
        QVERIFY(service.open(Server, Project, User));
        QTRY_VERIFY(service.available());
        const auto original = service.lease();
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(directory.filePath("cache.sqlite3"));
            QVERIFY(database.open());
            QSqlQuery writer(database);
            QVERIFY(writer.exec("BEGIN IMMEDIATE"));
            bool completed = false, okay = true;
            QVERIFY(service.forgetAccount(Server, Project, "99999999-9999-4999-8999-999999999999", this,
                                          [&](bool value) {
                                              okay = value;
                                              completed = true;
                                          }));
            QTRY_VERIFY(completed);
            QVERIFY(writer.exec("ROLLBACK"));
            QVERIFY(!okay);
            QVERIFY(!service.error().isEmpty());
            QCOMPARE(service.lease().owner, original.owner);
            QCOMPARE(service.lease().generation, original.generation);
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }

    void cleanupOpenedFromAClosedStoreInheritsItsCancellationCheck()
    {
        QTemporaryDir directory;
        {
            NativeCache seed(directory.path());
            QVERIFY(seed.open(Server, Project, User));
            QVERIFY(seed.put(seed.lease(), {NativeCache::Kind::History, Stream, Topic, {}}, rows()));
        }
        NativeCache closed(directory.path());
        int polls = 0;
        closed.setCancellationCheck([&] { return ++polls >= 3; });
        QVERIFY(!closed.forgetAccount(Server, Project, User));
        QVERIFY(polls >= 3);
        NativeCache check(directory.path());
        QVERIFY(check.open(Server, Project, User));
        QVERIFY(check.get(check.lease(), {NativeCache::Kind::History, Stream, Topic, {}}));
    }

    void storageCancellationStopsShrinkBeforePublishingASmallerBudget()
    {
        QTemporaryDir directory;
        NativeCache store(directory.path());
        QVERIFY(store.open(Server, Project, User));
        for (int i = 0; i < 32; ++i)
            QVERIFY(store.put(store.lease(),
                              {NativeCache::Kind::History, Stream, Topic, QByteArray::number(i)}, rows()));
        int polls = 0;
        store.setCancellationCheck([&] {
            ++polls;
            return true;
        });
        QVERIFY(!store.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
        QVERIFY(polls > 0);
        QVERIFY(!store.available());
        QVERIFY(store.diskUsageBytes() > NativeCache::MinimumDiskLimitBytes);
        store.setCancellationCheck({});
        QVERIFY(store.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));
        QVERIFY(store.open(Server, Project, User));
        QVERIFY(store.diskUsageBytes() <= NativeCache::MinimumDiskLimitBytes);
    }
};

QTEST_GUILESS_MAIN(NativeCacheServiceTest)
#include "native-cache-service-test.moc"
