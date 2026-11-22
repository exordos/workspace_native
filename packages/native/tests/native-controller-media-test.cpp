#include "image-decode-fixtures.h"
#include "support/heif-test-fixtures.h"
#include "support/native-test-harness.h"
#include "support/controller-media-fixtures.h"

#include <QBuffer>
#include <QImage>
#include <QScopeGuard>
#include <QSemaphore>
#include <QThread>
#include <QUrl>

class ControllerMediaTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    void mediaViewerSlotsStageBytesAndMarkFailures()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("PNG");
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.contains(mvFileB))
                return Response{QJsonObject{}, 500};
            if (path.endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/png";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QCOMPARE(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(1).toMap().value("state").toString(), QStringLiteral("load-error"), 4000);
        const auto ready = viewer->items().at(0).toMap();
        const QUrl url(ready.value("url").toString());
        QVERIFY(url.isLocalFile());
        QFile staged(url.toLocalFile());
        QVERIFY(staged.open(QIODevice::ReadOnly));
        QCOMPARE(staged.readAll(), bytes);
        QCOMPARE(ready.value("name").toString(), QStringLiteral("m1.png"));
        QCOMPARE(viewer->items().at(1).toMap().value("url").toString(), QString());
    }

    void mediaViewerRetiredSessionIgnoresLateSlotCompletions()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("PNG");
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QList<std::function<void()>> releasedA, releasedB;
        fixture.files = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/png";
                return result;
            }
            const auto isB = path.contains(mvFileB);
            Response result{QJsonObject{{"uuid", isB ? mvFileB : mvFileA}, {"stream_uuid", streamId},
                {"name", isB ? QStringLiteral("b.png") : QStringLiteral("a.png")},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
            auto &sink = isB ? releasedB : releasedA;
            result.defer = [&sink](std::function<void()> send) { sink.append(std::move(send)); };
            return result;
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList firstRows;
        firstRows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(firstRows, mvFileA));
        QTRY_VERIFY_WITH_TIMEOUT(!releasedA.isEmpty(), 2000);
        // Retire the session while its metadata response is still held...
        viewer->close();
        QVERIFY(!viewer->open());
        // ...and open a new session on another file before releasing it.
        QVariantList secondRows;
        secondRows << mediaViewerRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(secondRows, mvFileB));
        QCOMPARE(viewer->count(), 1);
        while (!releasedA.isEmpty())
            releasedA.takeFirst()();
        QTest::qWait(250);
        // A completion from the retired session must not touch the new slots.
        QCOMPARE(viewer->count(), 1);
        QCOMPARE(viewer->index(), 0);
        QCOMPARE(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("loading"));
        QTRY_VERIFY_WITH_TIMEOUT(!releasedB.isEmpty(), 2000);
        while (!releasedB.isEmpty())
            releasedB.takeFirst()();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        QCOMPARE(viewer->items().at(0).toMap().value("name").toString(), QStringLiteral("b.png"));
    }

    void mediaViewerClosesWhenSessionOrConversationEnds()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QVERIFY(viewer->open());
        fixture.controller.logout();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->open(), 2000);
    }

    // The gallery accepts image and video media references in document order
    // (workspace-message-list-media.lib.ts:20-31,44-66) and each slot carries
    // its media kind (workspace-media-viewer-item.lib.ts:22-26,48).
    void mediaViewerGalleryCarriesImageAndVideoSlotsInDocumentOrder()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA)
             << mediaViewerVideoRow("m2", mvFileB)
             << mediaViewerRow("m3", mvFileA)
             << mediaViewerVideoRow("m4", mvFileC);
        QVERIFY(viewer->openFromMessages(rows, mvFileB));
        QVERIFY(viewer->open());
        QCOMPARE(viewer->count(), 3);
        QCOMPARE(viewer->index(), 1);
        const auto items = viewer->items();
        QCOMPARE(items.at(0).toMap().value("uuid").toString(), mvFileA);
        QCOMPARE(items.at(0).toMap().value("kind").toString(), QStringLiteral("image"));
        QCOMPARE(items.at(1).toMap().value("uuid").toString(), mvFileB);
        QCOMPARE(items.at(1).toMap().value("kind").toString(), QStringLiteral("video"));
        QCOMPARE(items.at(2).toMap().value("uuid").toString(), mvFileC);
        QCOMPARE(items.at(2).toMap().value("kind").toString(), QStringLiteral("video"));
        // Opening on a video uuid anchors the start index on the video slot.
        QVERIFY(viewer->openFromMessages(rows, mvFileC));
        QCOMPARE(viewer->count(), 3);
        QCOMPARE(viewer->index(), 2);
        viewer->close();
    }

    // Non-clicked video slots do not preload on open
    // (workspace-media-viewer.hook.ts:205-212); they start when navigation
    // reaches them (the lazyLoadRef contract, hook :137-151,260-283).
    void mediaViewerVideoSlotsLoadLazilyWhenNavigationReachesThem()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("PNG");
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        int videoRequests = 0;
        fixture.files = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.contains(mvFileB))
                ++videoRequests;
            if (path.endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/png";
                return result;
            }
            const auto isB = path.contains(mvFileB);
            return Response{QJsonObject{{"uuid", isB ? mvFileB : mvFileA}, {"stream_uuid", streamId},
                {"name", isB ? QStringLiteral("clip.mp4") : QStringLiteral("m1.png")},
                {"content_type", isB ? "video/mp4" : "image/png"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerVideoRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QCOMPARE(viewer->count(), 2);
        QCOMPARE(viewer->items().at(1).toMap().value("state").toString(), QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        // The image slot loaded; the video slot is untouched until reached.
        QTest::qWait(250);
        QCOMPARE(videoRequests, 0);
        QCOMPARE(viewer->items().at(1).toMap().value("state").toString(), QStringLiteral("loading"));
        viewer->next();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(1).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        QVERIFY(videoRequests > 0);
        const auto video = viewer->items().at(1).toMap();
        QCOMPARE(video.value("kind").toString(), QStringLiteral("video"));
        QCOMPARE(video.value("name").toString(), QStringLiteral("clip.mp4"));
        const QUrl url(video.value("url").toString());
        QVERIFY(url.isLocalFile());
        viewer->close();
    }

    // The video onError path replaces the slot with display-error and clears
    // the url while the uuid stays downloadable
    // (media-viewer-overlay.ui.tsx:75-90).
    void mediaViewerMarkDisplayErrorFollowsTheReferenceReplaceContract()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("PNG");
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/png";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        // Out-of-range indexes and a closed session are no-ops.
        viewer->markSlotDisplayError(4);
        QCOMPARE(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"));
        viewer->markSlotDisplayError(0);
        const auto failed = viewer->items().at(0).toMap();
        QCOMPARE(failed.value("state").toString(), QStringLiteral("display-error"));
        QCOMPARE(failed.value("url").toString(), QString());
        QCOMPARE(failed.value("uuid").toString(), mvFileA);
        // A slot that is not ready keeps its state (the reference guards the
        // current item before replacing it).
        viewer->markSlotDisplayError(0);
        QCOMPARE(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("display-error"));
        viewer->close();
        viewer->markSlotDisplayError(0);
        QCOMPARE(viewer->count(), 0);
    }

    // Issue #638 sec 4.2: a HEIC image slot renders its staged original into
    // a decoded PNG on the render thread (QML Image cannot read ISO-BMFF);
    // the display URL points at the render, the staged original stays on
    // disk for download-by-uuid, and the metadata name is reported.
    void mediaViewerHeifSlotDisplaysDecodedRender()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/heic";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(),
            QStringLiteral("ready"), 8000);
        const auto ready = viewer->items().at(0).toMap();
        QCOMPARE(ready.value("name").toString(), QStringLiteral("photo.heic"));
        QCOMPARE(ready.value("uuid").toString(), mvFileA); // download-by-uuid stays enabled
        const QUrl display(ready.value("url").toString());
        QVERIFY(display.isLocalFile());
        QVERIFY(display.toLocalFile().endsWith(QStringLiteral("slot-0-heif.png"))); // render, not the original
        const QImage image(display.toLocalFile());
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(48, 32));
        QCOMPARE(image.pixelColor(24, 16), QColor(0x46, 0x82, 0xb4));
        viewer->close();
    }

    // A corrupt HEIC container lands on display-error with the uuid kept
    // (file-card parity: download stays the reference's escape hatch), never
    // on load-error and never ready.
    void mediaViewerCorruptHeifSlotReplacesWithDisplayErrorKeepingUuid()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicTruncatedFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/heic";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(),
            QStringLiteral("display-error"), 8000);
        const auto failed = viewer->items().at(0).toMap();
        QCOMPARE(failed.value("url").toString(), QString());
        QCOMPARE(failed.value("uuid").toString(), mvFileA);
        viewer->close();
    }

    // #670 review finding 2: the viewer HEIC render is bounded by the same
    // decode timeout as message previews. A stalled render settles the slot as
    // display-error (the uuid stays downloadable, file-card parity) while the
    // worker is still stuck, and the late successful render is discarded -
    // one settle per slot. The gate + shortened timeout make the 15 s path
    // deterministic (6x margin: 50 ms watchdog vs a >=300 ms guaranteed stall).
    void mediaViewerStalledHeifRenderTimesOutToDisplayError()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/heic";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        MediaViewerModel::setHeifRenderGateForTests([] { QThread::msleep(300); });
        const auto restore = qScopeGuard([] { MediaViewerModel::setHeifRenderGateForTests(nullptr); });
        viewer->setHeifRenderTimeoutMsForTests(50);
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(),
            QStringLiteral("display-error"), 2000); // settles while the render is still stalled
        QTest::qWait(450); // the stalled render completes (successfully) after the timeout
        const auto settled = viewer->items().at(0).toMap();
        QCOMPARE(settled.value("state").toString(), QStringLiteral("display-error")); // late render discarded
        QCOMPARE(settled.value("url").toString(), QString());
        QCOMPARE(settled.value("uuid").toString(), mvFileA); // download-by-uuid stays enabled
        viewer->close();
    }

    // #670 re-review: the render watchdog measures from the moment the serial
    // worker STARTS the render, never from the download. A slot queued behind
    // a stalled render must not burn its timeout in the queue: it stays
    // loading while queued and settles ready once its own render runs.
    void mediaViewerQueuedHeifRenderKeepsItsTimeoutUntilWorkerStart()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                if (request.target.path().contains(mvFileB))
                    QThread::msleep(150); // slot B joins the serial render queue after A has begun
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/heic";
                return result;
            }
            const auto uuid = request.target.path().contains(mvFileB) ? mvFileB : mvFileA;
            return Response{QJsonObject{{"uuid", uuid}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        int calls = 0;
        MediaViewerModel::setHeifRenderGateForTests([&calls] { if (++calls == 1) QThread::msleep(300); });
        const auto restore = qScopeGuard([] { MediaViewerModel::setHeifRenderGateForTests(nullptr); });
        viewer->setHeifRenderTimeoutMsForTests(100);
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        // Slot A: its render started and stalled, so it times out ~100 ms later.
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(),
            QStringLiteral("display-error"), 2000);
        // Slot B spent all of that queued behind the stalled worker. Its own
        // render begins when A's stall ends and succeeds within its timeout,
        // so it must end ready (at the previous head the download-armed timer
        // expired while B was still queued and pinned display-error).
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(1).toMap().value("state").toString(),
            QStringLiteral("ready"), 3000);
        viewer->close();
    }

    // #670 re-review round: the queue itself is bounded. The watchdog cannot
    // kill the serial worker, so a render stuck mid-decode must not strand
    // later slots in loading forever: a slot that never gets worker start
    // settles display-error on its own queue bound (uuid stays downloadable,
    // file-card parity), and its late render is discarded by the fences.
    void mediaViewerQueuedHeifRenderBehindStuckWorkerBoundsToDisplayError()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/heic";
                return result;
            }
            const auto uuid = request.target.path().contains(mvFileB) ? mvFileB : mvFileA;
            return Response{QJsonObject{{"uuid", uuid}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        // The serial worker is stuck for 600 ms inside slot A's render; B's
        // own render (once it finally starts) is fast.
        int calls = 0;
        MediaViewerModel::setHeifRenderGateForTests([&calls] { if (++calls == 1) QThread::msleep(600); });
        const auto restore = qScopeGuard([] { MediaViewerModel::setHeifRenderGateForTests(nullptr); });
        viewer->setHeifRenderTimeoutMsForTests(50);
        viewer->setHeifQueueTimeoutMsForTests(80);
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        // Slot A: started and stalled, its render watchdog settles it.
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(),
            QStringLiteral("display-error"), 2000);
        // Slot B: queued behind the stuck worker, never started - the queue
        // bound must settle it (~80 ms) long before the worker frees (~600 ms).
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(1).toMap().value("state").toString(),
            QStringLiteral("display-error"), 400);
        QTest::qWait(700); // the worker frees, B's late render runs and must be discarded
        const auto settled = viewer->items().at(1).toMap();
        QCOMPARE(settled.value("state").toString(), QStringLiteral("display-error")); // late render discarded
        QCOMPARE(settled.value("url").toString(), QString());
        QCOMPARE(settled.value("uuid").toString(), mvFileB); // download-by-uuid stays enabled
        viewer->close();
    }

    // #670 re-review round 3 (finding 4656047886): a healthy queue must not
    // expire late slots just because earlier healthy renders were being
    // processed. The bound measures worker STALL (gap between worker starts),
    // re-armed by every start - not each slot's absolute wait. Here three
    // renders are healthy (200 ms each, under the 800 ms render bound) but the
    // full drain (~600 ms) exceeds the 300 ms queue bound: the previous
    // per-slot absolute timer settled B and C to display-error before their
    // turn although the worker never stalled.
    void mediaViewerHealthyQueueDrainDoesNotExpireQueuedSlots()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/heic";
                return result;
            }
            const QString uuid = request.target.path().contains(mvFileB) ? mvFileB
                : request.target.path().contains(mvFileC) ? mvFileC : mvFileA;
            return Response{QJsonObject{{"uuid", uuid}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        // Every render is healthy: 200 ms, far below the 800 ms render bound.
        MediaViewerModel::setHeifRenderGateForTests([] { QThread::msleep(200); });
        const auto restore = qScopeGuard([] { MediaViewerModel::setHeifRenderGateForTests(nullptr); });
        viewer->setHeifRenderTimeoutMsForTests(800);
        viewer->setHeifQueueTimeoutMsForTests(300); // shorter than the full drain
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerRow("m2", mvFileB)
             << mediaViewerRow("m3", mvFileC);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        // A stall-bound settles nothing here: worker starts at ~0/200/400 ms
        // keep re-arming the clock; every slot must render ready even though
        // B and C waited longer than the 300 ms queue bound.
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(2).toMap().value("state").toString(),
            QStringLiteral("ready"), 2500);
        QCOMPARE(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"));
        QCOMPARE(viewer->items().at(1).toMap().value("state").toString(), QStringLiteral("ready"));
        viewer->close();
    }

    // --- Issue #79: copy image to clipboard (docs/NATIVE_DESKTOP_CLIPBOARD_IMAGE.md) ---
    // Mirrors the Electron clipboard:writeImage handler measured at workspace_ui
    // 61dacef7 (main.ts:1054-1067, cap main.ts:50): over-cap payloads are
    // rejected before decoding, undecodable buffers never touch the clipboard,
    // and every failure is silent - no toast, clipboard left unchanged.

    static void setClipboardImageSentinel()
    {
        QImage sentinel(2, 2, QImage::Format_RGB32);
        sentinel.fill(0xffff0000u);
        QGuiApplication::clipboard()->setImage(sentinel);
    }

    static bool clipboardSentinelIntact()
    {
        const QImage image = QGuiApplication::clipboard()->image();
        return image.size() == QSize(2, 2) && image.pixelColor(0, 0) == QColor(0xff, 0, 0);
    }

    static QByteArray clipboardCopyFixturePng()
    {
        for (const auto &fixture : NativeDecodeFixtures)
            if (QString::fromLatin1(fixture.format) == QLatin1String("png"))
                return QByteArray::fromBase64(QByteArray(fixture.base64));
        return {};
    }

    // Fix round R1 observable: reset the process peak-RSS high-water mark
    // (writing 5 to /proc/self/clear_refs) so a copy path that pulls an
    // oversized file whole into memory shows up as a measurable jump.
    static void resetPeakResidentMemory()
    {
        QFile clear(QStringLiteral("/proc/self/clear_refs"));
        if (clear.open(QIODevice::WriteOnly)) {
            clear.write("5");
            clear.close();
        }
    }

    static qint64 peakResidentBytes()
    {
        QFile status(QStringLiteral("/proc/self/status"));
        if (!status.open(QIODevice::ReadOnly))
            return 0;
        const auto lines = QString::fromLatin1(status.readAll()).split(QLatin1Char('\n'));
        for (const auto &line : lines)
            if (line.startsWith(QLatin1String("VmHWM:")))
                return line.sliced(6).trimmed().split(QLatin1Char(' ')).first().toLongLong() * 1024;
        return 0;
    }

    void copyMessageImageWritesImageBytesToClipboard()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray png = clipboardCopyFixturePng();
        QVERIFY(!png.isEmpty());
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = png;
                result.rawContentType = "image/png";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", png.size()}, {"hash", sha}}};
        };
        const auto body = QStringLiteral("Head\n\n![m1.png](urn:image:%1?name=m1.png&content_type=image%2Fpng&size=%2)\n\nTail")
            .arg(mvFileA).arg(png.size());
        const auto note = message(body, messageId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET") return {QJsonArray{note}};
            return {message("answer", returnedId), 201};
        };
        QVERIFY(fixture.start());
        QGuiApplication::clipboard()->clear();
        QVERIFY(fixture.controller.copyMessageImage(messageId));
        QTRY_VERIFY_WITH_TIMEOUT(!QGuiApplication::clipboard()->image().isNull(), 4000);
        // The locally generated fixture decodes to a solid 8x4 image.
        QCOMPARE(QGuiApplication::clipboard()->image().size(), QSize(8, 4));
    }

    void copyMessageImageUndecodableBytesLeaveClipboardUnchanged()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray junk("definitely not an image");
        int downloadHits = 0;
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(junk, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                ++downloadHits;
                Response result;
                result.rawBody = junk;
                result.rawContentType = "image/png";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", junk.size()}, {"hash", sha}}};
        };
        const auto body = QStringLiteral("Head\n\n![m1.png](urn:image:%1?name=m1.png&content_type=image%2Fpng&size=%2)\n\nTail")
            .arg(mvFileA).arg(junk.size());
        const auto note = message(body, messageId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET") return {QJsonArray{note}};
            return {message("answer", returnedId), 201};
        };
        QVERIFY(fixture.start());
        setClipboardImageSentinel();
        QVERIFY(fixture.controller.copyMessageImage(messageId));
        // The corrupt write must fail quietly: clipboard sentinel intact, no
        // error notice raised anywhere (reference shows no toast).
        QTRY_VERIFY_WITH_TIMEOUT(downloadHits >= 1, 4000);
        QTest::qWait(750);
        QVERIFY(clipboardSentinelIntact());
        QVERIFY(fixture.controller.error().isEmpty());
    }

    void copyMessageImageWithoutImageReferenceIsSilentNoop()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        fixture.files = [&](const Request &) { return Response{QJsonObject{}, 500}; };
        const auto note = message("plain text only, no media", messageId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET") return {QJsonArray{note}};
            return {message("answer", returnedId), 201};
        };
        QVERIFY(fixture.start());
        QGuiApplication::clipboard()->setText("sentinel text");
        QVERIFY(!fixture.controller.copyMessageImage(messageId));
        QVERIFY(!fixture.controller.copyMessageImage(QStringLiteral("00000000-0000-4000-8000-0000000000ff")));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("sentinel text"));
        // No media reference means no download attempt on the reference path.
        for (const auto &request : fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/"));
    }

    void clipboardImageBytesRejectsOversizedAndEmptyPayloads()
    {
        setClipboardImageSentinel();
        // A decodable png plus one byte over the 64 MiB cap: rejected before
        // any decode happens (main.ts:1084 order of checks).
        const QByteArray overCap = clipboardCopyFixturePng()
            + QByteArray(NativeController::ClipboardImageBytesLimit, '\0');
        QVERIFY(overCap.size() > NativeController::ClipboardImageBytesLimit);
        QVERIFY(!NativeController::clipboardImageBytes(overCap));
        QVERIFY(clipboardSentinelIntact());
        // Exactly at the cap passes the size gate but still fails to decode.
        QVERIFY(!NativeController::clipboardImageBytes(QByteArray(NativeController::ClipboardImageBytesLimit, 'x')));
        QVERIFY(clipboardSentinelIntact());
        // Empty payload behaves like nativeImage.isEmpty().
        QVERIFY(!NativeController::clipboardImageBytes(QByteArray()));
        QVERIFY(clipboardSentinelIntact());
        // The valid fixture passes and replaces the clipboard content.
        QVERIFY(NativeController::clipboardImageBytes(clipboardCopyFixturePng()));
        QCOMPARE(QGuiApplication::clipboard()->image().size(), QSize(8, 4));
    }

    // Fix round R1 (cid=4577117364, cid=4577117373): the shared bounded reader
    // behind both copy paths hands over at most cap + 1 bytes no matter how
    // much the source could still deliver.
    void clipboardBoundedReadStopsAtCapPlusOne()
    {
        class CountingDevice final : public QIODevice {
        public:
            explicit CountingDevice(qint64 total) : m_total(total) { open(QIODevice::ReadOnly); }
            qint64 provided() const { return m_provided; }
        protected:
            qint64 readData(char *data, qint64 maxLength) override
            {
                const qint64 count = qMin(maxLength, m_total - m_position);
                for (qint64 i = 0; i < count; ++i)
                    data[i] = '\x7f';
                m_position += count;
                m_provided += count;
                return count;
            }
            qint64 writeData(const char *, qint64 maxLength) override { return maxLength; }
            qint64 size() const override { return m_total; }
        private:
            qint64 m_total = 0, m_position = 0, m_provided = 0;
        };
        setClipboardImageSentinel();
        CountingDevice oversized(NativeController::ClipboardImageBytesLimit * 3);
        const auto bytes = NativeController::readClipboardBounded(oversized);
        QCOMPARE(bytes.size(), NativeController::ClipboardImageBytesLimit + 1);
        QCOMPARE(oversized.provided(), NativeController::ClipboardImageBytesLimit + 1);
        // The over-cap source is rejected by the shared write path having
        // cost only the cap plus one byte; the clipboard stays intact.
        QVERIFY(!NativeController::clipboardImageBytes(bytes));
        QVERIFY(clipboardSentinelIntact());
        // A payload exactly at the cap is handed over whole and untouched.
        CountingDevice exactlyAtCap(NativeController::ClipboardImageBytesLimit);
        QCOMPARE(NativeController::readClipboardBounded(exactlyAtCap).size(), NativeController::ClipboardImageBytesLimit);
        QCOMPARE(exactlyAtCap.provided(), NativeController::ClipboardImageBytesLimit);
    }

    void mediaViewerCopyWritesStagedImageBytes()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray png = clipboardCopyFixturePng();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = png;
                result.rawContentType = "image/png";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", png.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        QVERIFY(viewer->copyCurrentImage());
        QCOMPARE(QGuiApplication::clipboard()->image().size(), QSize(8, 4));
    }

    void mediaViewerCopyRejectsFailedAndVideoSlots()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        fixture.files = [&](const Request &) { return Response{QJsonObject{}, 500}; };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        setClipboardImageSentinel();
        // Closed viewer: nothing to copy.
        QVERIFY(!viewer->copyCurrentImage());
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("load-error"), 4000);
        QVERIFY(!viewer->copyCurrentImage());
        QVERIFY(clipboardSentinelIntact());
        viewer->close();
        // Video slots never offer the copy action (showCopy is image-only).
        QVariantList videoRows;
        videoRows << mediaViewerVideoRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(videoRows, mvFileB));
        QVERIFY(!viewer->copyCurrentImage());
        QVERIFY(clipboardSentinelIntact());
        viewer->close();
    }

    // Fix round R1 (cid=4577117364, cid=4577117373): the copy paths must bound
    // the bytes they read. A staged file over the 64 MiB clipboard cap is
    // rejected quietly, but reading it with readAll() first materialises the
    // whole payload - the test grows the staged slot file into a 200 MiB sparse
    // file (near-zero disk cost) and asserts peak RSS stays bounded, which an
    // unbounded readAll() cannot pass.
    void mediaViewerCopyRejectsOversizedStagedFileWithoutLoadingItWhole()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray png = clipboardCopyFixturePng();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                Response result;
                result.rawBody = png;
                result.rawContentType = "image/png";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", png.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        const auto staged = QUrl(viewer->items().at(0).toMap().value("url").toString()).toLocalFile();
        {
            QFile file(staged);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.resize(200 * 1024 * 1024));
        }
        setClipboardImageSentinel();
        resetPeakResidentMemory();
        const auto baseline = peakResidentBytes();
        // Over the cap: rejected silently, clipboard untouched, and the read is
        // bounded - the whole 200 MiB must never be loaded.
        QVERIFY(!viewer->copyCurrentImage());
        const auto growth = peakResidentBytes() - baseline;
        // The bound itself: a correct copy reads at most 64 MiB + 1 byte, so a
        // generous 96 MiB growth ceiling separates it from a 200 MiB readAll().
        QVERIFY2(growth < 96LL * 1024 * 1024,
            qPrintable(QStringLiteral("peak RSS grew by %1 MiB").arg(growth / (1024 * 1024))));
        QVERIFY(clipboardSentinelIntact());
        QVERIFY(fixture.controller.error().isEmpty());
        viewer->close();
    }

    // Fix round R1 (cid=4577117378): the download completion is bound to the
    // session that started it. A completion that lands after the identity is
    // switched must be discarded; it may not put the old session's image on the
    // system clipboard. The switch goes through Session::selectProject (the
    // same-token account-switch boundary), so the in-flight download still
    // completes after the identity moved on.
    void copyMessageImageCompletionAfterSessionEndIsDiscarded()
    {
        const QString otherProject = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        bool secondProject = false;
        Fixture fixture;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path() == "/api/core/v1/iam/projects/")
                response.data = QJsonArray{QJsonObject{{"uuid", projectId}, {"name", "First project"}, {"status", "ACTIVE"}},
                    QJsonObject{{"uuid", otherProject}, {"name", "Second project"}, {"status", "ACTIVE"}}};
            if (secondProject && request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject();
                raw["project_id"] = otherProject;
                response.data = raw;
            }
            return response;
        };
        fixture.streams = [&](const Request &) {
            if (secondProject) return Response{QJsonArray{}};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}}};
        };
        fixture.topics = [&](const Request &) {
            if (secondProject) return Response{QJsonArray{}};
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray png = clipboardCopyFixturePng();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        int downloadRequests = 0;
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                ++downloadRequests;
                Response result;
                result.rawBody = png;
                result.rawContentType = "image/png";
                result.delay = 500; // the completion lands after the identity switch
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", png.size()}, {"hash", sha}}};
        };
        const auto body = QStringLiteral("Head\n\n![m1.png](urn:image:%1?name=m1.png&content_type=image%2Fpng&size=%2)\n\nTail")
            .arg(mvFileA).arg(png.size());
        const auto note = message(body, messageId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET") return {QJsonArray{note}};
            return {message("answer", returnedId), 201};
        };
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.projects().size(), 2, 2000);
        fixture.controller.openProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.authenticated(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.streams().size() == 1, 2000);
        fixture.controller.selectStream(streamId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.topics().isEmpty(), 2000);
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.busy(), 2000);
        setClipboardImageSentinel();
        QVERIFY(fixture.controller.copyMessageImage(messageId));
        QTRY_VERIFY_WITH_TIMEOUT(downloadRequests >= 1, 4000);
        secondProject = true;
        fixture.session.selectProject(otherProject);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.authenticated(), 2000);
        // Let the delayed completion fire; it belongs to the retired identity.
        QTest::qWait(1500);
        // A stale completion must not put the previous session's image on the system clipboard.
        QVERIFY(clipboardSentinelIntact());
    }

    // Fix round R2 (cid=4577609973, cid=4577609975): the owner key alone is not
    // a completion fence. A project switch away and back re-creates the very
    // same creationOwnerKey, so a download dispatched before the detour would
    // pass the R1 fence after its session already retired and write its image
    // into the current clipboard. The controller now also fences on a session
    // generation that advances on every retirement boundary, and this test
    // stages the in-flight download with the mock's defer gate (no sleeps):
    // the response is delivered only after the session has returned to the
    // first project, and the reply's finish proves the delivery landed - so a
    // still-intact sentinel means the completion was discarded, not lost.
    void copyMessageImageCompletionAfterProjectRoundTripIsDiscarded()
    {
        const QString otherProject = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        bool secondProject = false;
        std::function<void()> releaseDownload;
        Fixture fixture;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path() == "/api/core/v1/iam/projects/")
                response.data = QJsonArray{QJsonObject{{"uuid", projectId}, {"name", "First project"}, {"status", "ACTIVE"}},
                    QJsonObject{{"uuid", otherProject}, {"name", "Second project"}, {"status", "ACTIVE"}}};
            if (secondProject && request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject();
                raw["project_id"] = otherProject;
                response.data = raw;
            }
            return response;
        };
        fixture.streams = [&](const Request &) {
            if (secondProject) return Response{QJsonArray{}};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}}};
        };
        fixture.topics = [&](const Request &) {
            if (secondProject) return Response{QJsonArray{}};
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray png = clipboardCopyFixturePng();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        int downloadRequests = 0;
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download")) {
                ++downloadRequests;
                Response result;
                result.rawBody = png;
                result.rawContentType = "image/png";
                // Hold only the in-flight copy dispatched below; the post-return
                // proof copy answers immediately.
                if (downloadRequests == 1)
                    result.defer = [&releaseDownload](std::function<void()> send) { releaseDownload = std::move(send); };
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId},
                {"name", "m1.png"}, {"content_type", "image/png"}, {"size_bytes", png.size()}, {"hash", sha}}};
        };
        const auto body = QStringLiteral("Head\n\n![m1.png](urn:image:%1?name=m1.png&content_type=image%2Fpng&size=%2)\n\nTail")
            .arg(mvFileA).arg(png.size());
        const auto note = message(body, messageId);
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "GET") return {QJsonArray{note}};
            return {message("answer", returnedId), 201};
        };
        // Never leave the mock socket with an undelivered deferred gate if an
        // assertion below aborts the test.
        auto releaseOnExit = qScopeGuard([&] { if (releaseDownload) releaseDownload(); });
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.projects().size(), 2, 2000);
        fixture.controller.openProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.authenticated(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.streams().size() == 1, 2000);
        fixture.controller.selectStream(streamId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.topics().isEmpty(), 2000);
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.busy(), 2000);
        setClipboardImageSentinel();
        QVERIFY(fixture.controller.copyMessageImage(messageId));
        QTRY_VERIFY_WITH_TIMEOUT(downloadRequests >= 1, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(bool(releaseDownload), 2000);
        // Track the in-flight download reply so the deferred delivery can be
        // proven (not assumed) once released.
        QPointer<QNetworkReply> inFlight;
        for (auto *reply : fixture.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path().endsWith("/actions/download")) inFlight = reply;
        QVERIFY(inFlight);
        QVERIFY(!inFlight->isFinished());
        // Detour to the second project and back: the owner key the completion
        // sees at the end is identical to the one captured at dispatch.
        secondProject = true;
        fixture.session.selectProject(otherProject);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.authenticated(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.busy(), 2000);
        secondProject = false;
        fixture.session.selectProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.authenticated(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.streams().size() == 1, 2000);
        // Deliver the retired operation's response now.
        releaseDownload();
        releaseDownload = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(inFlight.isNull() || inFlight->isFinished(), 4000);
        // The response landed but the completion belongs to a retired session
        // generation: the clipboard keeps the sentinel even though the owner
        // key matches again.
        QVERIFY(clipboardSentinelIntact());
        // Non-vacuity: a fresh copy under the current generation still writes
        // the image through the very same pipeline. First let the returned
        // session re-open the conversation the message lives in.
        fixture.controller.selectStream(streamId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.topics().isEmpty(), 2000);
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.messages().isEmpty(), 4000);
        QGuiApplication::clipboard()->clear();
        QVERIFY(fixture.controller.copyMessageImage(messageId));
        QTRY_VERIFY_WITH_TIMEOUT(!QGuiApplication::clipboard()->image().isNull(), 4000);
        QCOMPARE(QGuiApplication::clipboard()->image().size(), QSize(8, 4));
    }

    // Issue #680: a ready HEIC composer attachment must expose a decoded
    // preview produced by the bounded libheif decoder (reference parity:
    // docs/NATIVE_DESKTOP_HEIC_PREVIEWS.md sec 4.3). The tile can only render
    // what Qt's image reader understands, so the controller stages a decoded
    // PNG; the row keeps the ORIGINAL local path for sending either way.
    void composerReadyHeifAttachmentExposesDecodedPreview()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.method == "POST") {
                // Echo the multipart part type back (mirrors the backend), like the
                // FileClient-level HEIC test: the client compares the echo against its
                // own sniffed content type before accepting ready.
                QByteArray contentType("application/octet-stream");
                const auto lowered = request.rawBody.toLower();
                const auto namePos = lowered.indexOf("filename=\"photo.heic\"");
                if (namePos >= 0) {
                    const auto start = lowered.indexOf("content-type: ", namePos);
                    if (start >= 0) contentType = request.rawBody.mid(start + 14, request.rawBody.indexOf('\n', start) - start - 14);
                }
                return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                    {"content_type", QString::fromLatin1(contentType).trimmed()}, {"size_bytes", bytes.size()}, {"hash", sha}}, 201};
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto source = directory.filePath("photo.heic");
        QFile input(source);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        fixture.controller.attachFiles({QUrl::fromLocalFile(source)});
        QVariantMap ready;
        const auto readyRow = [&] {
            for (const auto &value : fixture.controller.uploadEntries()) {
                const auto row = value.toMap();
                if (row.value("status").toString() == "ready") { ready = row; return true; }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readyRow(), 5000);
        QCOMPARE(ready.value("image").toBool(), true);
        // Sending keeps reading the original bytes through the row's path.
        QCOMPARE(QUrl(ready.value("path").toString()).toLocalFile(), source);
        QString preview;
        const auto previewReady = [&] {
            for (const auto &value : fixture.controller.uploadEntries()) {
                const auto row = value.toMap();
                if (row.value("status").toString() != "ready") continue;
                preview = row.value("preview").toString();
                return !preview.isEmpty();
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(previewReady(), 8000);
        const QUrl previewUrl(preview);
        QVERIFY2(previewUrl.isLocalFile(), qPrintable(preview));
        const QImage image(previewUrl.toLocalFile());
        QVERIFY2(!image.isNull(), qPrintable(previewUrl.toLocalFile()));
        QCOMPARE(image.size(), QSize(48, 32));
        QCOMPARE(image.pixelColor(24, 16), QColor(0x46, 0x82, 0xb4));
    }

    // exordy round-2 finding (#680): preview selection classified by the file
    // NAME while decode scheduling consulted the echoed upload metadata. A PNG
    // renamed to photo.heic got neither its own preview (the name said HEIC)
    // nor a staged decode (the echo said image/png). Both decisions must
    // consult the same stored upload metadata.
    void composerReadyPngRenamedToHeicKeepsOriginalPreview()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QImage stamp(QSize(8, 4), QImage::Format_ARGB32);
        stamp.fill(QColor(0x12, 0x34, 0x56));
        QByteArray png;
        {
            QBuffer buffer(&png);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(stamp.save(&buffer, "PNG"));
        }
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.method == "POST") {
                // The backend classifies the stored bytes, not the filename:
                // echo the part's sniffed content type back.
                QByteArray contentType("application/octet-stream");
                const auto lowered = request.rawBody.toLower();
                const auto namePos = lowered.indexOf("filename=\"photo.heic\"");
                if (namePos >= 0) {
                    const auto start = lowered.indexOf("content-type: ", namePos);
                    if (start >= 0) contentType = request.rawBody.mid(start + 14, request.rawBody.indexOf('\n', start) - start - 14);
                }
                return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                    {"content_type", QString::fromLatin1(contentType).trimmed()}, {"size_bytes", png.size()}, {"hash", sha}}, 201};
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/png"}, {"size_bytes", png.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto source = directory.filePath("photo.heic");
        QFile input(source);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(png);
        input.close();
        const int baseline = NativeController::uploadPreviewDecodeStartsForTest();
        fixture.controller.attachFiles({QUrl::fromLocalFile(source)});
        QVariantMap ready;
        const auto readyRow = [&] {
            for (const auto &value : fixture.controller.uploadEntries()) {
                const auto row = value.toMap();
                if (row.value("status").toString() == "ready") { ready = row; return true; }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readyRow(), 5000);
        // The echo says PNG: the original IS renderable, so the row must
        // expose it directly instead of waiting for a decode nobody scheduled.
        QCOMPARE(QUrl(ready.value("preview").toString()).toLocalFile(), source);
        // And no HEIC decode was ever scheduled for this row.
        QCOMPARE(NativeController::uploadPreviewDecodeStartsForTest(), baseline);
    }

    // A corrupt container still uploads (the reference skips local probing for
    // HEIC and never caches a transport failure as content): the row stays
    // ready with an empty preview so the tile falls back to the file icon.
    void composerCorruptHeifAttachmentKeepsFileCardFallback()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicTruncatedFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.method == "POST") {
                // Echo the multipart part type back (mirrors the backend), like the
                // FileClient-level HEIC test: the client compares the echo against its
                // own sniffed content type before accepting ready.
                QByteArray contentType("application/octet-stream");
                const auto lowered = request.rawBody.toLower();
                const auto namePos = lowered.indexOf("filename=\"broken.heic\"");
                if (namePos >= 0) {
                    const auto start = lowered.indexOf("content-type: ", namePos);
                    if (start >= 0) contentType = request.rawBody.mid(start + 14, request.rawBody.indexOf('\n', start) - start - 14);
                }
                return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "broken.heic"},
                    {"content_type", QString::fromLatin1(contentType).trimmed()}, {"size_bytes", bytes.size()}, {"hash", sha}}, 201};
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "broken.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto source = directory.filePath("broken.heic");
        QFile input(source);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        fixture.controller.attachFiles({QUrl::fromLocalFile(source)});
        QVariantMap ready;
        const auto readyRow = [&] {
            for (const auto &value : fixture.controller.uploadEntries()) {
                const auto row = value.toMap();
                if (row.value("status").toString() == "ready") { ready = row; return true; }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readyRow(), 5000);
        // Generous window for the decode attempt to have run and failed.
        QTest::qWait(1500);
        QCOMPARE(ready.value("name").toString(), QStringLiteral("broken.heic"));
        QCOMPARE(ready.value("preview").toString(), QString());
    }

    // Removing the attachment drops its decoded preview: the staged file is
    // deleted with the row, and a late decode completion cannot resurrect it.
    void composerHeifPreviewDroppedWithTheAttachment()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.method == "POST") {
                // Echo the multipart part type back (mirrors the backend), like the
                // FileClient-level HEIC test: the client compares the echo against its
                // own sniffed content type before accepting ready.
                QByteArray contentType("application/octet-stream");
                const auto lowered = request.rawBody.toLower();
                const auto namePos = lowered.indexOf("filename=\"photo.heic\"");
                if (namePos >= 0) {
                    const auto start = lowered.indexOf("content-type: ", namePos);
                    if (start >= 0) contentType = request.rawBody.mid(start + 14, request.rawBody.indexOf('\n', start) - start - 14);
                }
                return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                    {"content_type", QString::fromLatin1(contentType).trimmed()}, {"size_bytes", bytes.size()}, {"hash", sha}}, 201};
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto source = directory.filePath("photo.heic");
        QFile input(source);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        fixture.controller.attachFiles({QUrl::fromLocalFile(source)});
        QVariantMap ready;
        const auto readyRow = [&] {
            for (const auto &value : fixture.controller.uploadEntries()) {
                const auto row = value.toMap();
                if (row.value("status").toString() == "ready") { ready = row; return true; }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readyRow(), 5000);
        QString preview;
        const auto previewReady = [&] {
            for (const auto &value : fixture.controller.uploadEntries()) {
                const auto row = value.toMap();
                if (row.value("status").toString() != "ready") continue;
                preview = row.value("preview").toString();
                return !preview.isEmpty();
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(previewReady(), 8000);
        const auto staged = QUrl(preview).toLocalFile();
        QVERIFY(QFile::exists(staged));
        fixture.controller.removeUpload(ready.value("id").toString());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.uploadEntries().isEmpty(), 2000);
        QVERIFY(!QFile::exists(staged));
    }

    // exordy review finding on #695: removing a HEIC attachment must cancel
    // its in-flight decode cooperatively. watcher->cancel() cannot stop a
    // running decode, so the worker observes a per-attachment token at every
    // stage boundary and inside libheif through cancel_decoding. The barrier
    // seam parks the worker inside its flight window, which makes the cancel
    // point deterministic instead of timing-sensitive.
    void composerHeifRemovalCancelsInFlightDecode()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes = heicFixture();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            if (request.method == "POST") {
                QByteArray contentType("application/octet-stream");
                const auto lowered = request.rawBody.toLower();
                const auto namePos = lowered.indexOf("filename=\"photo.heic\"");
                if (namePos >= 0) {
                    const auto start = lowered.indexOf("content-type: ", namePos);
                    if (start >= 0) contentType = request.rawBody.mid(start + 14, request.rawBody.indexOf('\n', start) - start - 14);
                }
                return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                    {"content_type", QString::fromLatin1(contentType).trimmed()}, {"size_bytes", bytes.size()}, {"hash", sha}}, 201};
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "photo.heic"},
                {"content_type", "image/heic"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto source = directory.filePath("photo.heic");
        QFile input(source);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        const int baseline = NativeController::uploadPreviewDecodeStartsForTest();
        QSemaphore workerReached;
        QSemaphore keepWorker;
        // A mid-test VERIFY abort must never leave the barrier installed: it
        // references stack semaphores the next test's worker would dangle on.
        const auto barrierGuard = qScopeGuard([&] {
            NativeController::setUploadPreviewBarrierForTest({});
            keepWorker.release();
        });
        NativeController::setUploadPreviewBarrierForTest([&] {
            workerReached.release();
            keepWorker.acquire();
        });
        fixture.controller.attachFiles({QUrl::fromLocalFile(source)});
        QVariantMap ready;
        const auto readyRow = [&] {
            for (const auto &value : fixture.controller.uploadEntries()) {
                const auto row = value.toMap();
                if (row.value("status").toString() == "ready") { ready = row; return true; }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readyRow(), 5000);
        // The preview worker is in flight now, parked at the barrier right
        // before the decode stage.
        QVERIFY(workerReached.tryAcquire(1, 5000));
        // Remove the row while the worker is inside its flight window.
        fixture.controller.removeUpload(ready.value("id").toString());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.uploadEntries().isEmpty(), 2000);
        keepWorker.release();
        // With the worker unblocked, no entry into the decode stage may ever
        // be observed: the token stopped the job at its first checkpoint.
        QTest::qWait(1000);
        NativeController::setUploadPreviewBarrierForTest({});
        QCOMPARE(NativeController::uploadPreviewDecodeStartsForTest(), baseline);
    }
};

QTEST_MAIN(ControllerMediaTest)
#include "native-controller-media-test.moc"
