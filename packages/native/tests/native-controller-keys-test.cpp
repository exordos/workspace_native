#include "support/native-test-harness.h"
#include "support/controller-identity-fixtures.h"
#include "support/controller-keys-fixtures.h"

class ControllerKeysTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Issue #244: key management (docs/NATIVE_DESKTOP_E2EE_KEY_MANAGEMENT.md) ----

    void chatLocalKeyInventoryEnumeratesVaultWithAnnouncementLabels()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.seedKey(KeyManagementFixture::KeyOld);
        // The vault returns receivedAt newest-first, so KeyOld (seeded last)
        // leads; the projection must move the current key to the top.
        f.controller.refreshChatLocalKeys();
        const auto inventory = f.controller.chatKeyInventory();
        QCOMPARE(inventory.value("state").toString(), QStringLiteral("ready"));
        QVERIFY(inventory.value("error").toString().isEmpty());
        const QStringList expectedUuids{KeyManagementFixture::KeyCurrent, KeyManagementFixture::KeyOld};
        QCOMPARE(f.inventoryUuids(), expectedUuids);
        const auto current = f.inventoryRows().at(0).toMap();
        QCOMPARE(current.value("keyUuid").toString(), KeyManagementFixture::KeyCurrent);
        QVERIFY(current.value("isCurrent").toBool());
        // Earliest key_announced date for the generation, not the newest one.
        QVERIFY(current.value("announcedAt").toString().startsWith(QStringLiteral("2026-01-01T08:00:00")));
        QVERIFY(QDateTime::fromString(current.value("addedAt").toString(), Qt::ISODateWithMs).isValid());
        const auto historical = f.inventoryRows().at(1).toMap();
        QCOMPARE(historical.value("keyUuid").toString(), KeyManagementFixture::KeyOld);
        QVERIFY(!historical.value("isCurrent").toBool());
        QVERIFY(historical.value("announcedAt").toString().startsWith(QStringLiteral("2025-12-01T00:00:00")));
        // A vault row without any announcement in loaded history keeps "" and
        // the QML falls back to "Not found in loaded history". Seeded last, it
        // is the newest receivedAt row and sits right behind the current key.
        const auto previousReceipt = QDateTime::fromString(
            historical.value("addedAt").toString(), Qt::ISODateWithMs);
        QVERIFY(previousReceipt.isValid());
        QTRY_VERIFY(QDateTime::currentDateTimeUtc() > previousReceipt);
        f.seedKey(KeyManagementFixture::KeyOrphan);
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.inventoryRows().size(), 3);
        const auto orphan = f.inventoryRows().at(1).toMap();
        QCOMPARE(orphan.value("keyUuid").toString(), KeyManagementFixture::KeyOrphan);
        QVERIFY(orphan.value("announcedAt").toString().isEmpty());
        // Joined-date labels match formatDateJoined in both locales, and the
        // key screen's createdLabel source is the current key's announcement.
        QCOMPARE(NativeController::formatChatKeyDate(QStringLiteral("2026-01-01T08:00:00.000"), "en"),
            QStringLiteral("Jan 1, 2026"));
        QCOMPARE(NativeController::formatChatKeyDate(QStringLiteral("2026-01-01T08:00:00.000"), "ru"),
            QStringLiteral("1 янв. 2026 г."));
        QCOMPARE(NativeController::formatChatKeyDate(QString(), "en"), QString());
        const auto encryption = f.controller.encryption();
        QCOMPARE(encryption.value("currentKeyUuid").toString(), KeyManagementFixture::KeyCurrent);
        QVERIFY(encryption.value("keyAnnouncedAt").toString().startsWith(QStringLiteral("2026-01-01T08:00:00")));
        QVERIFY(encryption.value("canRotate").toBool());
        // Deleting the current key stays allowed: recovery uses the request path.
        f.controller.deleteChatLocalKey(KeyManagementFixture::KeyCurrent);
        QVERIFY(f.controller.encryption().value("actionError").toString().isEmpty());
        const QStringList remainingUuids{KeyManagementFixture::KeyOrphan, KeyManagementFixture::KeyOld};
        QCOMPARE(f.inventoryUuids(), remainingUuids);
    }

    void chatLocalKeyInventoryErrorsUntilTheStoreIsReady()
    {
        KeyManagementFixture f;
        QVERIFY(f.start(false));
        // No durable store: the projection reports the reference error text and
        // the Retry action is a plain refresh once the store comes back.
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.controller.chatKeyInventory().value("state").toString(), QStringLiteral("error"));
        QCOMPARE(f.controller.chatKeyInventory().value("error").toString(),
            QStringLiteral("Could not load keys from this device."));
        QVERIFY(f.controller.chatKeyInventory().value("keys").toList().isEmpty());
        // Store opening publishes the loading state instead of an error.
        f.controller.setChatKeyStoreOpening(true);
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.controller.chatKeyInventory().value("state").toString(), QStringLiteral("loading"));
        f.controller.setChatKeyStoreOpening(false);
        f.attachVaultToController();
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.controller.chatKeyInventory().value("state").toString(), QStringLiteral("ready"));
        QCOMPARE(f.inventoryUuids(), QStringList{KeyManagementFixture::KeyCurrent});
    }

    void chatLocalKeyReissueAnnouncesAndFails()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.seedKey(KeyManagementFixture::KeyOld);
        f.controller.refreshChatLocalKeys();
        QVERIFY(f.controller.encryption().value("canRotate").toBool());
        f.deferAnnouncements = true;
        f.controller.reissueChatKey();
        QTRY_VERIFY_WITH_TIMEOUT(f.releaseAnnouncement != nullptr, 5000);
        // Pending covers the whole announcing window (verifying + sending).
        QVERIFY(f.controller.encryption().value("reissueBusy").toBool());
        const auto posted = f.announcementBodies.last().toObject();
        const auto payload = posted.value("payload").toObject();
        QCOMPARE(payload.value("kind").toString(), QStringLiteral("key_announced"));
        const auto announcedKey = payload.value("key_uuid").toString();
        QVERIFY(announcedKey != KeyManagementFixture::KeyCurrent);
        QVERIFY(announcedKey != KeyManagementFixture::KeyOld);
        QCOMPARE(posted.value("topic_uuid").toString(), secondTopicId);
        QCOMPARE(f.announcementPosts, 1);
        f.releaseAnnouncement();
        f.releaseAnnouncement = nullptr;
        f.deferAnnouncements = false;
        QTRY_VERIFY(!f.controller.encryption().value("reissueBusy").toBool());
        // The new generation is stored durably and shows up as a historical row
        // until the stream update moves currentEncryptionKey.
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, announcedKey}).value.bytes.size(), 32);
        QTRY_VERIFY(f.inventoryUuids().contains(announcedKey));
        QVERIFY(f.controller.encryption().value("actionError").toString().isEmpty());
        // Failure text: a rejected announcement clears pending and reports the
        // localized rotateKeyFailed text (the reference toast message).
        f.messagePostStatus = 500;
        f.controller.reissueChatKey();
        QTRY_VERIFY(f.controller.encryption().value("actionError").toString()
            == QStringLiteral("Could not reissue the encryption key. Please try again."));
        QVERIFY(!f.controller.encryption().value("reissueBusy").toBool());
        // Right gate: a member has no canRotate and reissue never posts.
        KeyManagementFixture member;
        member.viewerRole = "member";
        QVERIFY(member.start());
        QVERIFY(!member.controller.encryption().value("canRotate").toBool());
        member.controller.reissueChatKey();
        QCOMPARE(member.announcementPosts, 0);
        QVERIFY(!member.controller.encryption().value("reissueBusy").toBool());
    }

    void chatLocalKeyDeleteRemovesAndFails()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.seedKey(KeyManagementFixture::KeyOld);
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.inventoryUuids().size(), 2);
        f.controller.deleteChatLocalKey(KeyManagementFixture::KeyOld);
        QCOMPARE(f.inventoryUuids(), QStringList{KeyManagementFixture::KeyCurrent});
        // The removal is durable: a fresh enumeration keeps it gone, and the
        // vault no longer serves the key for decryption.
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.inventoryUuids(), QStringList{KeyManagementFixture::KeyCurrent});
        QVERIFY(f.vault.read(f.vault.lease(), {streamId, KeyManagementFixture::KeyOld}).value.bytes.isEmpty());
        // Failure texts: a malformed UUID and an unavailable store both land in
        // the localized deleteLocalKeyFailed message, matching the reference.
        f.controller.deleteChatLocalKey(QStringLiteral("not-a-uuid"));
        QCOMPARE(f.controller.encryption().value("actionError").toString(),
            QStringLiteral("Could not delete the key from this device. Please try again."));
        f.vault.close();
        f.controller.deleteChatLocalKey(KeyManagementFixture::KeyCurrent);
        QCOMPARE(f.controller.encryption().value("actionError").toString(),
            QStringLiteral("Could not delete the key from this device. Please try again."));
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.controller.chatKeyInventory().value("state").toString(), QStringLiteral("error"));
    }

    // R2 critical (review thread 4576750798): reissue stages the new generation
    // in the vault BEFORE the key_announced POST, and the inventory shows it
    // immediately. Deleting that staged key while the announcement is still in
    // flight loses the only copy if the server accepts the announcement later,
    // so an in-flight announcement must pin the key: deletion is refused while
    // the mark is set, the mark clears on a verified echo (accepted POST) or a
    // definite refusal (4xx), and transient outcomes (5xx) are not terminal.
    void chatLocalKeyRotationAnnouncementPinsTheStagedKey()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.controller.refreshChatLocalKeys();
        f.deferAnnouncements = true;
        f.controller.reissueChatKey();
        QTRY_VERIFY_WITH_TIMEOUT(f.releaseAnnouncement != nullptr, 5000);
        const auto staged = f.announcementBodies.last().toObject()
            .value("payload").toObject().value("key_uuid").toString();
        QVERIFY(!staged.isEmpty());
        // An inventory refresh while the announcement is in flight surfaces the
        // staged row (this is the review's scenario).
        f.controller.refreshChatLocalKeys();
        QVERIFY(f.inventoryUuids().contains(staged));
        f.controller.deleteChatLocalKey(staged);
        QVERIFY2(f.vault.read(f.vault.lease(), {streamId, staged}).value.bytes.size() == 32,
            "a staged rotation key must not be deletable while its announcement is in flight");
        QVERIFY(f.inventoryUuids().contains(staged));
        QCOMPARE(f.controller.encryption().value("actionError").toString(),
            QStringLiteral("This key is still being announced. It cannot be deleted yet."));
        // A verified echo resolves the mark: once the POST is accepted the pin
        // is lifted and the regular deletion rules apply to the new row.
        f.releaseAnnouncement();
        f.releaseAnnouncement = nullptr;
        f.deferAnnouncements = false;
        QTRY_VERIFY(!f.controller.encryption().value("reissueBusy").toBool());
        QTest::qWaitFor([&] { f.controller.deleteChatLocalKey(staged);
            return !f.inventoryUuids().contains(staged); }, 3000);
        QVERIFY2(!f.inventoryUuids().contains(staged),
            "deleting the announced key must be allowed once the echo verified");
        // A definite refusal (4xx) also resolves the mark.
        f.messagePostStatus = 403;
        f.controller.reissueChatKey();
        QTRY_VERIFY(f.controller.encryption().value("actionError").toString()
            == QStringLiteral("Could not reissue the encryption key. Please try again."));
        const auto refused = f.announcementBodies.last().toObject()
            .value("payload").toObject().value("key_uuid").toString();
        f.controller.refreshChatLocalKeys();
        QVERIFY(f.inventoryUuids().contains(refused));
        QVERIFY(QTest::qWaitFor([&] { f.controller.deleteChatLocalKey(refused);
            return !f.inventoryUuids().contains(refused); }, 3000));
        // A 5xx answer is not terminal: the mark stays set and the row pinned.
        f.messagePostStatus = 500;
        f.controller.reissueChatKey();
        QTRY_VERIFY(f.controller.encryption().value("actionError").toString()
            == QStringLiteral("Could not reissue the encryption key. Please try again."));
        const auto uncertain = f.announcementBodies.last().toObject()
            .value("payload").toObject().value("key_uuid").toString();
        f.controller.refreshChatLocalKeys();
        f.controller.deleteChatLocalKey(uncertain);
        QVERIFY(f.inventoryUuids().contains(uncertain));
        QCOMPARE(f.controller.encryption().value("actionError").toString(),
            QStringLiteral("This key is still being announced. It cannot be deleted yet."));
    }

    // R2 critical (review thread 4576750798), restart half: the in-flight pin
    // lives in the durable vault, so a restarted controller must keep refusing
    // the deletion, and a success that landed while the process was gone must
    // resolve the pin through the announcement echo (GET of the deterministic
    // message uuid) on the next inventory reveal.
    void chatLocalKeyRotationPinSurvivesRestartAndDelayedEchoLands()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.controller.refreshChatLocalKeys();
        f.deferAnnouncements = true;
        f.controller.reissueChatKey();
        QTRY_VERIFY_WITH_TIMEOUT(f.releaseAnnouncement != nullptr, 5000);
        const auto body = f.announcementBodies.last().toObject();
        const auto staged = body.value("payload").toObject().value("key_uuid").toString();
        const auto announcement = body.value("uuid").toString();
        QVERIFY(!QUuid(announcement).isNull());
        // The response never returns and the process restarts over the same
        // durable vault: the staged row is still present and still pinned.
        KeyManagementRestart restart(f);
        QVERIFY(restart.start());
        restart.controller.refreshChatLocalKeys();
        QVERIFY(restart.inventoryUuids().contains(staged));
        restart.controller.deleteChatLocalKey(staged);
        QVERIFY2(restart.vault.read(restart.vault.lease(), {streamId, staged}).value.bytes.size() == 32,
            "the rotation pin must survive the restart: the stage lives in the durable vault");
        QCOMPARE(restart.controller.encryption().value("actionError").toString(),
            QStringLiteral("This key is still being announced. It cannot be deleted yet."));
        // The announcement actually landed while the process was gone. The
        // reveal-driven recheck GETs the message and must resolve the pin, so
        // the delayed success still lands and the row returns to normal rules.
        f.serverAnnouncedMessages.insert(announcement.toLower(), staged);
        QVERIFY(QTest::qWaitFor([&] { restart.controller.refreshChatLocalKeys();
            restart.controller.deleteChatLocalKey(staged);
            return !restart.inventoryUuids().contains(staged); }, 5000));
        QVERIFY(restart.controller.encryption().value("actionError").toString().isEmpty());
    }

    // R3 critical (review thread 4577108882): the durable dispatch marker is
    // the gate in front of the key_announced POST. If the marker write fails,
    // the POST must never be dispatched: an undispatched pin is exactly the
    // record a GET-404 recheck is allowed to clear, so posting anyway lets a
    // recheck delete the pin (and then the key) of an in-flight announcement.
    void chatLocalKeyRotationDispatchMarkerFailureAbortsBeforeThePost()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.inventoryUuids().size(), 1);
        const auto postsBefore = f.announcementPosts;
        f.vault.failDispatchMarker = true;
        f.controller.reissueChatKey();
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.encryption().value("reissueBusy").toBool(), 5000);
        // The reissue surfaces a retryable failure and never touches the wire.
        QCOMPARE(f.controller.encryption().value("actionError").toString(),
            QStringLiteral("Could not reissue the encryption key. Please try again."));
        QCOMPARE(f.announcementPosts, postsBefore);
        // No pin is left marked dispatched: the never-dispatched record is
        // provably safe to retire with the aborted attempt, so nothing pinned
        // survives this failure.
        const auto stages = f.vault.rotationStages(f.vault.lease());
        QCOMPARE(stages.failure, NativeChatKeyVault::Failure::None);
        QCOMPARE(stages.values.size(), 0);
        // The staged key itself is kept (never destroyed) and, with nothing
        // dispatched for it, returns to the regular deletion rules.
        f.controller.refreshChatLocalKeys();
        const auto rows = f.inventoryUuids();
        QCOMPARE(rows.size(), 2);
        const auto staged = rows.value(0) == KeyManagementFixture::KeyCurrent ? rows.value(1) : rows.value(0);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, staged}).value.bytes.size(), 32);
        f.controller.deleteChatLocalKey(staged);
        QVERIFY(!f.inventoryUuids().contains(staged));
    }

    // R3 critical (review thread 4577108882), recheck half: the GET-404 pass
    // may only clear a pin whose DURABLE record still says dispatch was never
    // attempted. A scan snapshot is not enough - the marker may have landed
    // (the POST is effectively in flight) while the recheck GET was running.
    void chatLocalKeyRotationAbsenceRecheckOnlyClearsProvablyUndispatchedRecords()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.controller.refreshChatLocalKeys();
        const QString inflight = "88888888-8888-4888-9888-888888888888";
        const QString inflightAnnouncement = "aaaaaaa2-2222-4222-8222-222222222222";
        QCOMPARE(f.vault.put(f.vault.lease(), {streamId, inflight}, QByteArray(32, char(9)), 0),
            NativeChatKeyVault::Failure::None);
        QCOMPARE(f.vault.putRotationStage(f.vault.lease(), {inflightAnnouncement, streamId, inflight,
            false, QDateTime::currentMSecsSinceEpoch()}), NativeChatKeyVault::Failure::None);
        // Arm the recheck pass and hold its GET: at snapshot time the record
        // durably says "never dispatched".
        f.deferMessageChecks = true;
        f.controller.refreshChatLocalKeys();
        QTRY_VERIFY_WITH_TIMEOUT(f.releaseMessageCheck != nullptr, 5000);
        // The dispatch marker lands after the snapshot - exactly the durable
        // state a POST that has left the process leaves behind.
        QCOMPARE(f.vault.markRotationStageDispatched(f.vault.lease(), {streamId, inflight}),
            NativeChatKeyVault::Failure::None);
        f.releaseMessageCheck();
        f.releaseMessageCheck = nullptr;
        // The 404 arrives against a stale snapshot. The pin's durable record
        // now says dispatch-attempted, so absence must not clear it.
        QVERIFY(QTest::qWaitFor([&] {
            const auto pin = f.vault.readRotationStage(f.vault.lease(), {streamId, inflight});
            return pin.failure == NativeChatKeyVault::Failure::None && pin.value->missingEvidence > 0;
        }, 5000));
        const auto pin = f.vault.readRotationStage(f.vault.lease(), {streamId, inflight});
        QVERIFY2(pin.failure == NativeChatKeyVault::Failure::None && pin.value && pin.value->dispatched,
            "a 404 recheck must never clear a pin whose durable record says dispatch-attempted");
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, inflight}).value.bytes.size(), 32);
        f.controller.deleteChatLocalKey(inflight);
        QCOMPARE(f.controller.encryption().value("actionError").toString(),
            QStringLiteral("This key is still being announced. It cannot be deleted yet."));
        // A provably-never-dispatched record (crash before the POST left any
        // process) still clears on absence: the tightening must not strand it.
        f.deferMessageChecks = false;
        const QString ghost = "77777777-7777-4777-8777-777777777777";
        const QString ghostAnnouncement = "aaaaaaa1-1111-4111-8111-111111111111";
        QCOMPARE(f.vault.put(f.vault.lease(), {streamId, ghost}, QByteArray(32, char(8)), 0),
            NativeChatKeyVault::Failure::None);
        QCOMPARE(f.vault.putRotationStage(f.vault.lease(), {ghostAnnouncement, streamId, ghost,
            false, QDateTime::currentMSecsSinceEpoch()}), NativeChatKeyVault::Failure::None);
        f.controller.refreshChatLocalKeys();
        QVERIFY(QTest::qWaitFor([&] {
            return f.vault.readRotationStage(f.vault.lease(), {streamId, ghost}).failure
                == NativeChatKeyVault::Failure::MissingKey;
        }, 5000));
    }

    // R3 medium (review thread 4577108887): a crash after dispatch-marking
    // leaves a pin no 404 may clear, and 32 such pins would exhaust the
    // device-wide rotation-stage budget forever. The accepted #234 protocol
    // applies: absence evidence accumulates durably and saturates into a
    // dormant quarantine WITHOUT destroying the key; the user gets the
    // terminal authority - an unresolved inventory, a check that re-adopts
    // on positive proof, and an informed explicit discard that frees the
    // pin only after an explicit confirmation. 404 alone never destroys.
    void chatLocalKeyDormantRotationStagesAreUserResolvable()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.controller.refreshChatLocalKeys();
        const QString keyX = "c0c0c0c0-0000-4000-8000-0000000000c0";
        const QString keyY = "c1c1c1c1-1111-4111-8111-1111111111c1";
        const QString announcementX = "a2a2a2a2-2222-4222-8222-2222222222a2";
        const QString announcementY = "a3a3a3a3-3333-4333-8333-3333333333a3";
        for (const auto &key : {keyX, keyY})
            QCOMPARE(f.vault.put(f.vault.lease(), {streamId, key}, QByteArray(32, char(6)), 0),
                NativeChatKeyVault::Failure::None);
        const auto stage = [&](const QString &key, const QString &announcement) {
            QCOMPARE(f.vault.putRotationStage(f.vault.lease(), {announcement, streamId, key,
                false, QDateTime::currentMSecsSinceEpoch()}), NativeChatKeyVault::Failure::None);
            QCOMPARE(f.vault.markRotationStageDispatched(f.vault.lease(), {streamId, key}),
                NativeChatKeyVault::Failure::None);
        };
        stage(keyX, announcementX);
        stage(keyY, announcementY);
        // Five conclusive-absence scans saturate the durable evidence: both
        // dispatched pins go dormant, keys fully intact, pins still held.
        // The durable evidence write is the scan's completion signal: wait on
        // it (not just on wire arrival) so each refresh arms a fresh pass.
        for (int pass = 0; pass < 5; ++pass) {
            f.controller.refreshChatLocalKeys();
            QVERIFY2(QTest::qWaitFor([&] {
                for (const auto &key : {keyX, keyY}) {
                    const auto pin = f.vault.readRotationStage(f.vault.lease(), {streamId, key});
                    if (pin.failure != NativeChatKeyVault::Failure::None || !pin.value
                        || pin.value->missingEvidence < std::min(pass + 1, 5)) return false;
                }
                return true;
            }, 5000), qPrintable(QStringLiteral("pass %1 did not saturate both pins").arg(pass)));
        }
        for (const auto &key : {keyX, keyY}) {
            const auto pin = f.vault.readRotationStage(f.vault.lease(), {streamId, key});
            QVERIFY(pin.failure == NativeChatKeyVault::Failure::None && pin.value);
            QVERIFY2(pin.value->dispatched && pin.value->missingEvidence >= 5,
                "dispatch-attempted stages under repeated conclusive absence must saturate into dormancy");
            QCOMPARE(f.vault.read(f.vault.lease(), {streamId, key}).value.bytes.size(), 32);
        }
        // The unresolved inventory: identity and bookkeeping only - never key
        // material - and both dormant stages are listed with their evidence.
        const auto rows = f.controller.unresolvedRotationStages();
        QCOMPARE(rows.size(), 2);
        for (const auto &row : rows) {
            const auto object = row.toObject();
            QCOMPARE(object.value("stream").toString(), streamId);
            QVERIFY(object.keys().contains("keyUuid") && object.keys().contains("dormant"));
            QVERIFY(object.value("dormant").toBool());
            QVERIFY(object.value("dispatchAttempted").toBool());
            QVERIFY(object.value("missingEvidence").toInt() >= 5);
        }
        // Check while the announcement is still absent: not terminal, nothing
        // changes - the key and the pin stay byte-for-byte.
        QSignalSpy resolvedSpy(&f.controller, &NativeController::rotationStageResolved);
        QVERIFY(resolvedSpy.isValid());
        const auto expectSettle = [&resolvedSpy](const int before, const QString &expectedOutcome,
            const QString &where) {
            const bool settled = QTest::qWaitFor([&] {
                return resolvedSpy.count() > before
                    && resolvedSpy.last().at(2).toString() == expectedOutcome;
            }, 5000);
            if (!settled) {
                QString outcomes;
                for (const auto &args : resolvedSpy) outcomes += args.at(2).toString() + QLatin1Char(',');
                qWarning("%s: spy outcomes [%s] (before %d)", qPrintable(where), qPrintable(outcomes), before);
            }
            return settled;
        };
        int mark = resolvedSpy.count();
        f.controller.checkRotationStage(streamId, keyX);
        QVERIFY(expectSettle(mark, QStringLiteral("unresolved"), "check absent"));
        QCOMPARE(resolvedSpy.last().at(0).toString(), streamId);
        QCOMPARE(resolvedSpy.last().at(1).toString().toLower(), keyX);
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, keyX}).value);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, keyX}).value.bytes.size(), 32);
        // The announcement finally lands: the check verifies the echo and
        // re-adopts - the pin frees on positive proof, the key is kept.
        f.serverAnnouncedMessages.insert(announcementX, keyX);
        mark = resolvedSpy.count();
        f.controller.checkRotationStage(streamId, keyX);
        QVERIFY(expectSettle(mark, QStringLiteral("resolved"), "check landed"));
        QCOMPARE(f.vault.readRotationStage(f.vault.lease(), {streamId, keyX}).failure,
            NativeChatKeyVault::Failure::MissingKey);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, keyX}).value.bytes.size(), 32);
        // Informed discard for the other dormant pin: unconfirmed is refused
        // and changes nothing; the explicit confirmation frees only the pin.
        mark = resolvedSpy.count();
        f.controller.discardRotationStage(streamId, keyY, false);
        QVERIFY(expectSettle(mark, QStringLiteral("unresolved"), "discard unconfirmed"));
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, keyY}).value);
        mark = resolvedSpy.count();
        f.controller.discardRotationStage(streamId, keyY, true);
        QVERIFY(expectSettle(mark, QStringLiteral("discarded"), "discard confirmed"));
        QCOMPARE(f.vault.readRotationStage(f.vault.lease(), {streamId, keyY}).failure,
            NativeChatKeyVault::Failure::MissingKey);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, keyY}).value.bytes.size(), 32);
        f.controller.refreshChatLocalKeys();
        QVERIFY(f.inventoryUuids().contains(keyY));
        // The capacity case: a full 32-record budget of dormant pins rejects a
        // new rotation with its own message instead of silently losing the
        // action, and one user discard frees the way out.
        QVector<QPair<QString, QString>> filled;
        while (f.vault.rotationStages(f.vault.lease()).values.size()
            < NativeChatKeyVault::MaximumRotationStages) {
            const auto key = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            const auto announcement = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            QCOMPARE(f.vault.put(f.vault.lease(), {streamId, key}, QByteArray(32, char(5)), 0),
                NativeChatKeyVault::Failure::None);
            QCOMPARE(f.vault.putRotationStage(f.vault.lease(), {announcement, streamId, key,
                false, QDateTime::currentMSecsSinceEpoch()}), NativeChatKeyVault::Failure::None);
            QCOMPARE(f.vault.markRotationStageDispatched(f.vault.lease(), {streamId, key}),
                NativeChatKeyVault::Failure::None);
            QCOMPARE(f.vault.setRotationStageMissingEvidence(f.vault.lease(), {streamId, key}, 5),
                NativeChatKeyVault::Failure::None);
            filled.append({streamId, key});
        }
        QCOMPARE(f.vault.rotationStages(f.vault.lease()).values.size(),
            NativeChatKeyVault::MaximumRotationStages);
        const auto postsBefore = f.announcementPosts;
        f.controller.reissueChatKey();
        QTRY_VERIFY_WITH_TIMEOUT(!f.controller.encryption().value("reissueBusy").toBool(), 5000);
        QCOMPARE(f.announcementPosts, postsBefore);
        QCOMPARE(f.controller.encryption().value("actionError").toString(),
            QStringLiteral("All rotation announcement slots are held by unresolved rotations. "
                           "Resolve one of them and try again."));
        mark = resolvedSpy.count();
        f.controller.discardRotationStage(streamId, filled.first().second, true);
        QVERIFY(expectSettle(mark, QStringLiteral("discarded"), "capacity discard"));
        f.controller.reissueChatKey();
        QTRY_VERIFY_WITH_TIMEOUT(f.announcementPosts > postsBefore, 5000);
    }

    // Issue #294: a key_announced POST in flight when the same-owner access
    // token is replaced retires with the API generation: the shared request
    // wrapper drops its callback, so nothing ever cleared m_chatReissueBusy
    // and every later reissue returned silently. The renewal must release the
    // transient busy state while the uncertain durable pin stays pinned
    // (fail-closed #244 rules untouched); a NEW reissue must dispatch and
    // settle under the renewed session; the retired POST's late answer must
    // not settle or re-set the newer operation; and the surviving pin stays
    // verifiable by the regular check (no blind resend).
    void chatKeyReissueRenewalReleasesBusyAndReissueRetriesUnderRenewedSession()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.seedKey(KeyManagementFixture::KeyOld);
        f.controller.refreshChatLocalKeys();
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        const auto inner = f.server.handler;
        f.server.handler = [inner, hold, releases](const Request &request) -> Response {
            auto response = inner(request);
            if (hold->load() && request.method == "POST"
                && request.target.path() == QLatin1String("/api/workspace/v1/messenger/messages/")
                && request.body.value("payload").toObject().value("kind").toString()
                    == QLatin1String("key_announced"))
                response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
            return response;
        };
        f.controller.reissueChatKey();
        QVERIFY2(QTest::qWaitFor([&] { return releases->size() >= 1; }, 5000),
            "the reissue never dispatched its key_announced POST");
        QVERIFY(f.controller.encryption().value("reissueBusy").toBool());
        const auto first = f.announcementBodies.last().toObject();
        const auto firstKey = first.value("payload").toObject().value("key_uuid").toString();
        const auto firstAnnouncement = first.value("uuid").toString();
        QCOMPARE(f.vault.readRotationStage(f.vault.lease(), {streamId, firstKey}).failure,
            NativeChatKeyVault::Failure::None); // the durable pin exists while in flight
        // A normal same-owner token replacement retires the in-flight POST.
        f.api.setAccessToken("synthetic-renewed-access-token");
        QVERIFY2(QTest::qWaitFor([&] {
            return !f.controller.encryption().value("reissueBusy").toBool();
        }, 5000), "token renewal left the reissue busy flag set forever");
        QVERIFY(f.controller.encryption().value("actionError").toString().isEmpty());
        // Durable fail-closed retention: cancellation proves nothing about the
        // announcement, so the pin and the staged key stay intact...
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, firstKey}).value);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, firstKey}).value.bytes.size(), 32);
        // ...and the interrupted pin is user-visible again (the stranded busy
        // marker no longer hides it from the unresolved inventory).
        bool listed = false;
        for (const auto &row : f.controller.unresolvedRotationStages())
            listed = listed || row.toObject().value("keyUuid").toString() == firstKey;
        QVERIFY2(listed, "the pin interrupted by the renewal stayed hidden from the unresolved inventory");
        // A later operation can proceed under the renewed session: the user
        // reopens the key screen (the refresh re-verifies the catalog, exactly
        // like the issue's trigger), then reissues again.
        f.controller.refresh();
        QTest::qWait(500);
        f.controller.reissueChatKey();
        QVERIFY2(QTest::qWaitFor([&] { return releases->size() >= 2; }, 5000),
            "reissue could not dispatch again after the token renewal");
        QVERIFY(f.controller.encryption().value("reissueBusy").toBool());
        const auto second = f.announcementBodies.last().toObject();
        const auto secondKey = second.value("payload").toObject().value("key_uuid").toString();
        // The retired POST's answer lands while the newer reissue is running:
        // it must neither settle it nor clear the newer durable pin.
        (*releases)[0]();
        QTest::qWait(300);
        QVERIFY(f.controller.encryption().value("reissueBusy").toBool());
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, firstKey}).value);
        // The newer reissue's own answer then settles it cleanly.
        hold->store(false);
        (*releases)[1]();
        QVERIFY2(QTest::qWaitFor([&] {
            return !f.controller.encryption().value("reissueBusy").toBool();
        }, 5000), "the reissue retried after the renewal never settled");
        QVERIFY(f.controller.encryption().value("actionError").toString().isEmpty());
        QCOMPARE(f.vault.readRotationStage(f.vault.lease(), {streamId, secondKey}).failure,
            NativeChatKeyVault::Failure::MissingKey); // accepted announcement frees its pin
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, firstKey}).value);
        // The surviving pin is verified by the regular check - never resent.
        f.serverAnnouncedMessages.insert(firstAnnouncement, firstKey);
        QSignalSpy resolvedSpy(&f.controller, &NativeController::rotationStageResolved);
        f.controller.checkRotationStage(streamId, firstKey);
        QVERIFY2(QTest::qWaitFor([&] { return resolvedSpy.size() >= 1; }, 5000),
            "the check of the pin that survived the cancellation never settled");
        QCOMPARE(resolvedSpy.last().at(2).toString(), QStringLiteral("resolved"));
        QCOMPARE(f.vault.readRotationStage(f.vault.lease(), {streamId, firstKey}).failure,
            NativeChatKeyVault::Failure::MissingKey);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, firstKey}).value.bytes.size(), 32);
    }

    // Issue #294: the automatic rotation-resolution pass holds its single-flight
    // marker only across its own GET chain, so a renewal that retires the GET
    // strands m_rotationResolveActive forever - no later pass could ever start.
    // The renewal must release the marker and re-arm the abandoned pass so the
    // landed announcement is adopted without a restart.
    void chatRotationAutoResolutionInterruptedByRenewalReArmsAndAdopts()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyOld);
        const QString announcement = "b1b1b1b1-1111-4111-8111-1111111111b1";
        const QString key = KeyManagementFixture::KeyOld;
        QCOMPARE(f.vault.putRotationStage(f.vault.lease(), {announcement, streamId, key,
            false, QDateTime::currentMSecsSinceEpoch()}), NativeChatKeyVault::Failure::None);
        QCOMPARE(f.vault.markRotationStageDispatched(f.vault.lease(), {streamId, key}),
            NativeChatKeyVault::Failure::None);
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        const auto inner = f.server.handler;
        const auto announcementTarget =
            QStringLiteral("/api/workspace/v1/messenger/messages/") + announcement;
        f.server.handler = [inner, hold, releases, announcementTarget](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path() == announcementTarget) {
                auto response = inner(request);
                if (hold->load())
                    response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
                return response;
            }
            return inner(request);
        };
        f.controller.refreshChatLocalKeys(); // arms the automatic resolution pass
        QVERIFY2(QTest::qWaitFor([&] { return releases->size() >= 1; }, 5000),
            "the automatic rotation-resolution pass never dispatched its recheck GET");
        f.api.setAccessToken("synthetic-renewed-access-token");
        (*releases)[0](); // the retired pass's answer lands on a retired generation
        QVERIFY2(QTest::qWaitFor([&] { return releases->size() >= 2; }, 5000),
            "the rotation-resolution pass retired by the token renewal was never re-armed");
        (*releases)[1](); // the re-armed pass's own inconclusive answer ends that pass
        // The announcement lands after the renewal: a later inventory refresh
        // can start a fresh pass and adopt it - no restart, no blind resend.
        // (The catalog refresh first mirrors the session refresh completing,
        // which re-verifies the catalog the inventory pass reads through.)
        f.serverAnnouncedMessages.insert(announcement, key);
        hold->store(false);
        f.controller.refresh();
        QTest::qWait(500);
        f.controller.refreshChatLocalKeys();
        QVERIFY2(QTest::qWaitFor([&] {
            return f.vault.readRotationStage(f.vault.lease(), {streamId, key}).failure
                == NativeChatKeyVault::Failure::MissingKey;
        }, 5000), "a rotation-resolution pass started after the renewal never adopted the landed announcement");
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, key}).value.bytes.size(), 32);
    }

    // Issue #294: a manual rotation check interrupted by a token renewal keeps
    // its per-stage running marker: the interrupted check never reports, and
    // every later Check for that stage is silently ignored. The renewal must
    // settle the interrupted check as unresolved (pin and key intact - the
    // answer was never conclusive) and a later check must work again.
    void chatRotationManualCheckInterruptedByRenewalReleasesRunningMarkerAndRetries()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyOld);
        const QString announcement = "b2b2b2b2-2222-4222-8222-2222222222b2";
        const QString key = KeyManagementFixture::KeyOld;
        QCOMPARE(f.vault.putRotationStage(f.vault.lease(), {announcement, streamId, key,
            false, QDateTime::currentMSecsSinceEpoch()}), NativeChatKeyVault::Failure::None);
        QCOMPARE(f.vault.markRotationStageDispatched(f.vault.lease(), {streamId, key}),
            NativeChatKeyVault::Failure::None);
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        const auto inner = f.server.handler;
        f.server.handler = [inner, hold, releases](const Request &request) -> Response {
            if (request.method == "GET"
                && request.target.path().startsWith(QLatin1String("/api/workspace/v1/messenger/messages/"))) {
                auto response = inner(request);
                if (hold->load())
                    response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
                return response;
            }
            return inner(request);
        };
        QSignalSpy resolvedSpy(&f.controller, &NativeController::rotationStageResolved);
        f.controller.checkRotationStage(streamId, key);
        QVERIFY2(QTest::qWaitFor([&] { return releases->size() >= 1; }, 5000),
            "the manual rotation check never dispatched its GET");
        QCOMPARE(resolvedSpy.size(), 0); // the deferred GET keeps the check running
        f.api.setAccessToken("synthetic-renewed-access-token");
        QVERIFY2(QTest::qWaitFor([&] { return resolvedSpy.size() == 1; }, 5000),
            "a manual rotation check interrupted by the token renewal never settled");
        QCOMPARE(resolvedSpy.last().at(0).toString(), streamId);
        QCOMPARE(resolvedSpy.last().at(1).toString().toLower(), key);
        QCOMPARE(resolvedSpy.last().at(2).toString(), QStringLiteral("unresolved"));
        QVERIFY(resolvedSpy.last().at(3).toString().contains(QStringLiteral("staged key was kept")));
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, key}).value);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, key}).value.bytes.size(), 32);
        (*releases)[0](); // the retired generation's answer must not settle a second time
        QTest::qWait(300);
        QCOMPARE(resolvedSpy.size(), 1);
        // The interrupted check is retryable under the renewed session.
        f.serverAnnouncedMessages.insert(announcement, key);
        hold->store(false);
        f.controller.checkRotationStage(streamId, key);
        QVERIFY2(QTest::qWaitFor([&] { return resolvedSpy.size() == 2; }, 5000),
            "the running marker stranded by the renewal blocked every later manual check");
        QCOMPARE(resolvedSpy.last().at(2).toString(), QStringLiteral("resolved"));
        QCOMPARE(f.vault.readRotationStage(f.vault.lease(), {streamId, key}).failure,
            NativeChatKeyVault::Failure::MissingKey);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, key}).value.bytes.size(), 32);
    }

    // Issue #294: sign-out across a deferred key_announced POST must not strand
    // the reissue busy flag, and the next sign-in of the same owner must be
    // able to reissue again (the durable pin of the retired attempt survives
    // the sign-out fail-closed).
    void chatKeyReissueSignOutSettlesAndReLoginAllowsFreshReissue()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.seedKey(KeyManagementFixture::KeyOld);
        f.controller.refreshChatLocalKeys();
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto releases = std::make_shared<std::vector<std::function<void()>>>();
        const auto inner = f.server.handler;
        f.server.handler = [inner, hold, releases](const Request &request) -> Response {
            auto response = inner(request);
            if (hold->load() && request.method == "POST"
                && request.target.path() == QLatin1String("/api/workspace/v1/messenger/messages/")
                && request.body.value("payload").toObject().value("kind").toString()
                    == QLatin1String("key_announced"))
                response.defer = [releases](std::function<void()> send) { releases->push_back(std::move(send)); };
            return response;
        };
        f.controller.reissueChatKey();
        QVERIFY2(QTest::qWaitFor([&] { return releases->size() >= 1; }, 5000),
            "the reissue never dispatched its key_announced POST");
        QVERIFY(f.controller.encryption().value("reissueBusy").toBool());
        const auto first = f.announcementBodies.last().toObject();
        const auto firstKey = first.value("payload").toObject().value("key_uuid").toString();
        f.controller.logout(); // clearSession bumps the API generation too
        QVERIFY2(QTest::qWaitFor([&] {
            return !f.controller.encryption().value("reissueBusy").toBool();
        }, 5000), "sign-out left the reissue busy flag set");
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, firstKey}).value);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, firstKey}).value.bytes.size(), 32);
        // Sign back in as the same owner; the retired answer lands with no effect.
        QVERIFY2(f.start(false), "the synthetic re-login failed");
        f.controller.useChatKeys(&f.vault);
        (*releases)[0]();
        QTest::qWait(300);
        QVERIFY(!f.controller.encryption().value("reissueBusy").toBool());
        // A fresh reissue dispatches and settles under the new session.
        hold->store(false);
        f.controller.reissueChatKey();
        QVERIFY2(QTest::qWaitFor([&] { return f.announcementPosts >= 2; }, 5000),
            "no fresh reissue could be dispatched after sign-out and re-login");
        QVERIFY2(QTest::qWaitFor([&] {
            return !f.controller.encryption().value("reissueBusy").toBool();
        }, 5000), "the fresh reissue after re-login never settled");
        QVERIFY(f.controller.encryption().value("actionError").toString().isEmpty());
        QVERIFY(f.vault.readRotationStage(f.vault.lease(), {streamId, firstKey}).value);
    }

    // R2 medium (review thread 4576750804): the details screen's "Request this
    // key again" button is enabled for a saved generation, but the request
    // primitive short-circuited while the vault held the bytes and produced
    // neither a request nor a success status. The reference re-requests a saved
    // generation - a fresh key_request POST while the saved copy stays intact.
    void chatLocalKeyRequestSavedGenerationSendsAndKeepsTheSavedBytes()
    {
        KeyManagementFixture f;
        QVERIFY(f.start());
        f.seedKey(KeyManagementFixture::KeyCurrent);
        f.seedKey(KeyManagementFixture::KeyOld);
        f.controller.refreshChatLocalKeys();
        QCOMPARE(f.inventoryUuids().size(), 2);
        f.controller.requestChatKey(KeyManagementFixture::KeyOld);
        QVERIFY2(QTest::qWaitFor([&] { return f.requestPosts == 1; }, 5000),
            "requesting a saved generation must post a key_request instead of short-circuiting");
        const auto posted = f.requestBodies.last().toObject();
        const auto payload = posted.value("payload").toObject();
        QCOMPARE(payload.value("kind").toString(), QStringLiteral("key_request"));
        QCOMPARE(payload.value("key_uuid").toString(), KeyManagementFixture::KeyOld);
        QCOMPARE(posted.value("stream_uuid").toString(), streamId);
        // The request reaches the reference "requested" success state, and the
        // saved bytes are untouched (the request stays bindable for a later grant).
        QTRY_VERIFY(f.controller.encryption().value("state").toString() == QStringLiteral("requested"));
        QCOMPARE(f.controller.encryption().value("keyUuid").toString(), KeyManagementFixture::KeyOld);
        QCOMPARE(f.vault.read(f.vault.lease(), {streamId, KeyManagementFixture::KeyOld}).value.bytes.size(), 32);
        QCOMPARE(f.inventoryUuids().size(), 2);
    }
};

QTEST_MAIN(ControllerKeysTest)
#include "native-controller-keys-test.moc"
