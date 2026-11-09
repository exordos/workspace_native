#include "native-drafts.h"
#include "native-app.h"
#include "native-cache-service.h"
#include "mock-server.h"

#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace workspace;
using namespace workspace::test;
using Failure = NativeDraftService::Failure;

namespace {
const QString ownerA(64, 'a'), ownerB(64, 'b');
DraftRecord stored()
{
    DraftRecord r; r.uuid = returnedId; r.stream = streamId; r.topic = topicId;
    r.snapshot = messageId; r.text = "Previous input"; r.updatedAt = 1000; return r;
}
bool seed(const QString &path)
{
    NativeDraftStore store(path);
    return store.open(ownerA) == Failure::None && store.put(stored()) == Failure::None;
}
QString numberedTopic(int index)
{
    return QString("99000000-0000-4000-8000-%1").arg(index, 12, 16, QLatin1Char('0'));
}
bool seedMany(const QString &path, const QString &owner, int count)
{
    NativeDraftStore store(path);
    if (store.open(owner) != Failure::None) return false;
    QVector<DraftRecord> records;
    for (int i = 0; i < count; ++i) {
        auto record = stored(); record.uuid = numberedTopic(i); record.topic = numberedTopic(i);
        record.text = "Body " + QString::number(i); records.append(record);
    }
    return store.apply(records, {}) == Failure::None;
}
}

class NativeDraftsTest final : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }

    void cleanDraftBodiesHaveABoundedRecordCacheAndReloadFromDisk()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA);
        for (int i = 0; i < NativeDrafts::MaximumCachedRecords + 12; ++i) {
            const auto topic = numberedTopic(i); drafts.select(streamId, topic);
            QTRY_VERIFY(drafts.ready(streamId, topic));
            QVERIFY(drafts.edit(streamId, topic, "Body " + QString::number(i)));
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        }
        QTRY_VERIFY(drafts.residency().cachedRecords <= NativeDrafts::MaximumCachedRecords);
        drafts.select(streamId, numberedTopic(0)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(0)));
        QCOMPARE(drafts.text(), "Body 0");
    }

    void cleanDraftPayloadHasAByteBudgetIndependentOfRecordCount()
    {
        QTemporaryDir directory;
        {
            NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
            for (int i = 0; i < 48; ++i) {
                auto record = stored(); record.uuid = numberedTopic(i); record.topic = numberedTopic(i);
                record.text = QString::number(i) + QString(39995, QChar(0x754c));
                QCOMPARE(store.put(record), Failure::None);
            }
        }
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service); drafts.setOwner(ownerA);
        for (int i = 0; i < 48; ++i) {
            const auto topic = numberedTopic(i); drafts.select(streamId, topic); QTRY_VERIFY(drafts.ready(streamId, topic));
            QVERIFY(drafts.text().startsWith(QString::number(i)));
        }
        QTRY_VERIFY(drafts.residency().cachedBytes <= NativeDrafts::MaximumCachedBytes);
        QVERIFY(drafts.residency().cachedRecords < 48);
    }

    void emptyConversationMetadataAlsoHasABound()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service); drafts.setOwner(ownerA);
        for (int i = 0; i < NativeDrafts::MaximumCachedRoutes + 12; ++i) {
            const auto topic = numberedTopic(i); drafts.select(streamId, topic); QTRY_VERIFY(drafts.ready(streamId, topic));
            QVERIFY(drafts.text().isEmpty());
        }
        QTRY_VERIFY(drafts.residency().cachedRoutes <= NativeDrafts::MaximumCachedRoutes);
    }

    void unchangedEmptyEditsDoNotAccumulateIdleMetadata()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QTest::qWait(5);
        for (int i = 0; i < NativeDrafts::MaximumCachedRoutes + 12; ++i)
            QVERIFY(drafts.edit(streamId, numberedTopic(i), ""));
        QTRY_VERIFY(drafts.residency().cachedRoutes <= NativeDrafts::MaximumCachedRoutes);
    }

    void lastSnapshotCopyReleasesResidencyAndDiskRestorationStillWorks()
    {
        QTemporaryDir directory; QVERIFY(seedMany(directory.path(), ownerA, 76));
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service); drafts.setOwner(ownerA);
        drafts.select(streamId, numberedTopic(0)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(0)));
        auto token = drafts.capture(streamId, numberedTopic(0)); auto copy = token; token = {};
        for (int i = 1; i < 76; ++i) {
            drafts.select(streamId, numberedTopic(i)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(i)));
        }
        QVERIFY(drafts.ready(streamId, numberedTopic(0))); QCOMPARE(drafts.text(streamId, numberedTopic(0)), "Body 0");
        copy = {};
        QTRY_VERIFY(!drafts.ready(streamId, numberedTopic(0)));
        QVERIFY(drafts.residency().cachedRecords <= NativeDrafts::MaximumCachedRecords);
        drafts.select(streamId, numberedTopic(0)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(0)));
        QCOMPARE(drafts.text(), "Body 0");
    }

    void snapshotConsumptionAndNewerSameTextSurviveCachePressure()
    {
        QTemporaryDir directory; QVERIFY(seedMany(directory.path(), ownerA, 76));
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service); drafts.setOwner(ownerA);
        drafts.select(streamId, numberedTopic(0)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(0)));
        const auto token = drafts.capture(streamId, numberedTopic(0));
        QVERIFY(drafts.edit(streamId, numberedTopic(0), "Intermediate"));
        QVERIFY(drafts.edit(streamId, numberedTopic(0), "Body 0"));
        for (int i = 1; i < 76; ++i) {
            drafts.select(streamId, numberedTopic(i)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(i)));
        }
        QVERIFY(!drafts.consume(token)); QCOMPARE(drafts.text(streamId, numberedTopic(0)), "Body 0");
        QVERIFY(drafts.consume(drafts.capture(streamId, numberedTopic(0))));
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        service.shutdown(); QTRY_VERIFY(service.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QVERIFY(!store.latest(streamId, numberedTopic(0)).record);
    }

    void emptyAttachmentConversationCanBeRetainedThroughMetadataPressure()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId); QTRY_VERIFY(drafts.ready(streamId, topicId));
        auto token = drafts.capture(streamId, topicId); QVERIFY(token.residency); QVERIFY(token.uuid.isEmpty());
        for (int i = 0; i < NativeDrafts::MaximumCachedRoutes + 12; ++i) {
            drafts.select(streamId, numberedTopic(i)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(i)));
        }
        QVERIFY(drafts.ready(streamId, topicId)); QVERIFY(drafts.retained(streamId, topicId));
        QVERIFY(drafts.edit(streamId, topicId, "Late attachment caption")); token = {};
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        drafts.select(streamId, topicId); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QCOMPARE(drafts.text(), "Late attachment caption");
    }

    void failedWritesAndProtectedInputAreNotEvictedByAnotherOwner()
    {
        QTemporaryDir directory; QVERIFY(seedMany(directory.path(), ownerB, 76));
        NativeDraftStore lock(directory.path()); QCOMPARE(lock.open(ownerA), Failure::None);
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service); drafts.setOwner(ownerA);
        drafts.select(streamId, topicId); QVERIFY(drafts.edit(streamId, topicId, "Uncommitted input"));
        QVERIFY(drafts.edit(streamId, secondTopicId, ":::local test-only text:::"));
        QTRY_VERIFY(drafts.failure() != Failure::None);
        drafts.setOwner(ownerB);
        for (int i = 0; i < 76; ++i) {
            drafts.select(streamId, numberedTopic(i)); QTRY_VERIFY(drafts.ready(streamId, numberedTopic(i)));
            QVERIFY(!drafts.text().contains("Uncommitted"));
        }
        drafts.setOwner(ownerA); drafts.select(streamId, topicId); QCOMPARE(drafts.text(), "Uncommitted input");
        QCOMPARE(drafts.text(streamId, secondTopicId), ":::local test-only text:::");
        lock.close(); bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        service.shutdown(); QTRY_VERIFY(service.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.latest(streamId, topicId).record->text, "Uncommitted input");
        QVERIFY(!store.latest(streamId, secondTopicId).record);
    }

    void snapshotReleaseAfterModelDestructionIsSafe()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts::Token token;
        {
            NativeDrafts drafts(&service); drafts.setOwner(ownerA); drafts.select(streamId, topicId);
            QVERIFY(drafts.edit(streamId, topicId, "Retain me")); token = drafts.capture(streamId, topicId);
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        }
        token = {}; QCoreApplication::processEvents();
    }

    void inputIsCoalescedAndDurableAcrossTwoLifetimes()
    {
        QTemporaryDir directory;
        const QString exact = "  Текст 😀\nwith trailing space  ";
        {
            NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
            drafts.setOwner(ownerA); drafts.select(streamId, topicId);
            QTRY_VERIFY_WITH_TIMEOUT(drafts.ready(streamId, topicId), 2000);
            QVERIFY(drafts.edit(streamId, topicId, "first"));
            QTest::qWait(100);
            QVERIFY(drafts.edit(streamId, topicId, exact));
            QTest::qWait(100);
            bool read = false; NativeDraftStore::Read result;
            service.latest(service.lease(), streamId, topicId, this, [&](auto r) { result = r; read = true; });
            QTRY_VERIFY_WITH_TIMEOUT(read, 1000); QVERIFY(!result.record);
            bool flushed = false, okay = false;
            drafts.flush(this, [&](bool success) { okay = success; flushed = true; });
            QTRY_VERIFY_WITH_TIMEOUT(flushed, 2000); QVERIFY(okay);
        }
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId);
        QTRY_VERIFY_WITH_TIMEOUT(drafts.ready(streamId, topicId), 2000);
        QCOMPARE(drafts.text(), exact);
    }

    void inputBeforeHydrationCannotBeReplacedByAnOldDiskRow()
    {
        QTemporaryDir directory; QVERIFY(seed(directory.path()));
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId);
        QVERIFY(drafts.edit(streamId, topicId, "Newer input"));
        QTRY_VERIFY_WITH_TIMEOUT(drafts.ready(streamId, topicId), 2000);
        QCOMPARE(drafts.text(), "Newer input");
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    }

    void leadingWhitespaceRemainsEditableBeforePublicTextExists()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QVERIFY(drafts.edit(streamId, topicId, " ")); QCOMPARE(drafts.text(), " ");
        QVERIFY(drafts.edit(streamId, topicId, "  ")); QCOMPARE(drafts.text(), "  ");
        QVERIFY(drafts.edit(streamId, topicId, "  code"));
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        QVERIFY(drafts.edit(streamId, topicId, "\t")); QCOMPARE(drafts.text(), "\t");
        QVERIFY(drafts.edit(streamId, topicId, "\tnew line"));
        done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        service.shutdown(); QTRY_VERIFY(service.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.latest(streamId, topicId).record->text, "\tnew line");
    }

    void typingThenClearingBeforeHydrationDoesNotResurrectOldText()
    {
        QTemporaryDir directory; QVERIFY(seed(directory.path()));
        {
            NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
            drafts.setOwner(ownerA); drafts.select(streamId, topicId);
            QVERIFY(drafts.edit(streamId, topicId, "New input"));
            QVERIFY(drafts.edit(streamId, topicId, ""));
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY(drafts.text().isEmpty());
        }
        NativeDraftStore reopened(directory.path()); QCOMPARE(reopened.open(ownerA), Failure::None);
        const auto result = reopened.latest(streamId, topicId);
        QCOMPARE(result.failure, Failure::None); QVERIFY(!result.record);
    }

    void ownerAndConversationSwitchesRetainCapturedWrites()
    {
        QTemporaryDir directory;
        {
            NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
            drafts.setOwner(ownerA); drafts.select(streamId, topicId);
            QVERIFY(drafts.edit(streamId, topicId, "A first"));
            drafts.select(streamId, secondTopicId); QVERIFY(drafts.edit(streamId, secondTopicId, "A second"));
            drafts.setOwner(ownerB); drafts.select(streamId, topicId);
            QVERIFY(drafts.text().isEmpty()); QVERIFY(drafts.edit(streamId, topicId, "B only"));
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 3000); QCOMPARE(drafts.text(), "B only");
        }
        NativeDraftStore a(directory.path()); QCOMPARE(a.open(ownerA), Failure::None);
        QCOMPARE(a.latest(streamId, topicId).record->text, "A first");
        QCOMPARE(a.latest(streamId, secondTopicId).record->text, "A second"); a.close();
        NativeDraftStore b(directory.path()); QCOMPARE(b.open(ownerB), Failure::None);
        QCOMPARE(b.latest(streamId, topicId).record->text, "B only");
    }

    void anotherStoredDraftCanBeRestoredAfterSendingAndRevisiting()
    {
        QTemporaryDir directory;
        {
            NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
            auto older = stored(); older.uuid = secondTopicId; older.updatedAt = 500; older.text = "Older distinct draft";
            QCOMPARE(store.apply({older, stored()}, {}), Failure::None);
        }
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId);
        QTRY_VERIFY(drafts.ready(streamId, topicId)); QCOMPARE(drafts.text(), stored().text);
        QVERIFY(drafts.edit(streamId, topicId, "Latest input"));
        QVERIFY(drafts.consume(drafts.capture(streamId, topicId)));
        drafts.select(streamId, secondTopicId); drafts.select(streamId, topicId);
        QTRY_VERIFY(drafts.ready(streamId, topicId)); QCOMPARE(drafts.text(), "Older distinct draft");
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
    }

    void sendConsumesOnlyTheExactCapturedSnapshot()
    {
        QTemporaryDir directory;
        {
            NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
            drafts.setOwner(ownerA); drafts.select(streamId, topicId);
            QVERIFY(drafts.edit(streamId, topicId, "A")); const auto token = drafts.capture(streamId, topicId);
            QVERIFY(drafts.edit(streamId, topicId, "B")); QVERIFY(drafts.edit(streamId, topicId, "A"));
            QVERIFY(!drafts.consume(token)); QCOMPARE(drafts.text(), "A");
            auto exact = drafts.capture(streamId, topicId);
            drafts.select(streamId, secondTopicId);
            QVERIFY(drafts.consume(exact)); QVERIFY(drafts.text(streamId, topicId).isEmpty());
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
        }
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QVERIFY(!store.latest(streamId, topicId).record);
        const auto rows = store.list(); QCOMPARE(rows.rows.size(), 1); QVERIFY(rows.rows.first().consumed);
    }

    void protectedInputRemainsInMemoryAndClearsAnEarlierPublicDraft()
    {
        QTemporaryDir directory; QVERIFY(seed(directory.path()));
        const QString privateText = ":::private test-only fragment:::";
        {
            NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
            drafts.setOwner(ownerA); drafts.select(streamId, topicId);
            QVERIFY(drafts.edit(streamId, topicId, privateText));
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QCOMPARE(drafts.text(), privateText);
            QVERIFY(drafts.capture(streamId, topicId).uuid.isEmpty());
        }
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QVERIFY(!store.latest(streamId, topicId).record);
        for (const auto &row : store.list().rows) {
            const auto record = store.read(row.uuid); QVERIFY(record.record);
            QVERIFY(!record.record->text.contains(privateText));
        }
    }

    void invalidPublicInputCannotReportSuccessfulFlush()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId);
        const QString oversized(40001, 'x'); QVERIFY(!drafts.edit(streamId, topicId, oversized));
        QCOMPARE(drafts.text(), oversized); QCOMPARE(drafts.failure(), Failure::InvalidRecord);
        bool done = false, okay = true; drafts.flush(this, [&](bool success) { okay = success; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY(!okay); QCOMPARE(drafts.text(), oversized);
        QVERIFY(drafts.edit(streamId, topicId, "Corrected"));
        done = false; drafts.flush(this, [&](bool success) { okay = success; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY(okay);
    }

    void unavailableStorePreservesInputAndCanRetry()
    {
        QTemporaryDir directory; NativeDraftStore lock(directory.path()); QCOMPARE(lock.open(ownerA), Failure::None);
        NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        drafts.setOwner(ownerA); drafts.select(streamId, topicId); QVERIFY(drafts.edit(streamId, topicId, "Keep me"));
        bool done = false, okay = true; drafts.flush(this, [&](bool success) { okay = success; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY(!okay); QCOMPARE(drafts.text(), "Keep me");
        lock.close(); done = false; drafts.retry();
        drafts.flush(this, [&](bool success) { okay = success; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY(okay);
    }

    void destroyedFlushReceiverDoesNotCancelDurability()
    {
        QTemporaryDir directory;
        {
            NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
            drafts.setOwner(ownerA); drafts.select(streamId, topicId); QVERIFY(drafts.edit(streamId, topicId, "Retained"));
            auto *receiver = new QObject; bool called = false;
            drafts.flush(receiver, [&](bool) { called = true; }); delete receiver;
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY(!called);
        }
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.latest(streamId, topicId).record->text, "Retained");
    }

    void applicationShutdownWaitsForCurrentInputAndBothWorkers()
    {
        QTemporaryDir directory;
        const auto draftPath = directory.filePath("drafts");
        NativeDraftService service(draftPath); NativeDrafts drafts(&service);
        NativeCacheService cache(directory.filePath("cache"));
        drafts.setOwner(ownerA); drafts.select(streamId, topicId);
        QVERIFY(drafts.edit(streamId, topicId, "Quit before debounce"));
        bool done = false, okay = false;
        shutdownNativeStorage(drafts, service, cache, this, [&](bool success) {
            okay = success; done = true;
            QVERIFY(service.stopped()); QVERIFY(cache.stopped());
        });
        QVERIFY(!done); QTRY_VERIFY_WITH_TIMEOUT(done, 3000); QVERIFY(okay);
        NativeDraftStore store(draftPath); QCOMPARE(store.open(ownerA), Failure::None);
        QCOMPARE(store.latest(streamId, topicId).record->text, "Quit before debounce");
    }

    void applicationShutdownFailureLeavesInputAndWorkersForRetry()
    {
        QTemporaryDir directory; const auto draftPath = directory.filePath("drafts");
        NativeDraftStore lock(draftPath); QCOMPARE(lock.open(ownerA), Failure::None);
        NativeDraftService service(draftPath); NativeDrafts drafts(&service);
        NativeCacheService cache(directory.filePath("cache"));
        drafts.setOwner(ownerA); drafts.select(streamId, topicId); QVERIFY(drafts.edit(streamId, topicId, "Keep on failure"));
        bool done = false, okay = true;
        shutdownNativeStorage(drafts, service, cache, this, [&](bool success) { okay = success; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2000); QVERIFY(!okay);
        QVERIFY(!service.stopped()); QVERIFY(!cache.stopped()); QCOMPARE(drafts.text(), "Keep on failure");
        lock.close(); done = false;
        shutdownNativeStorage(drafts, service, cache, this, [&](bool success) { okay = success; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000); QVERIFY(okay); QVERIFY(service.stopped()); QVERIFY(cache.stopped());
    }
};
QTEST_GUILESS_MAIN(NativeDraftsTest)
#include "native-drafts-test.moc"
