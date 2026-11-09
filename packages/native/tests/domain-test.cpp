#include "domain.h"
#include "mock-server.h"

#include <QTest>
#include <QFile>
#include <QCryptographicHash>

using namespace workspace;
using namespace workspace::test;

class DomainTest final : public QObject {
    Q_OBJECT
private slots:
    void ownedDraftSnapshotKeepsCanonicalTextRevisionAndMicrosecondOrder()
    {
        const QJsonObject raw{{"uuid", returnedId}, {"project_id", projectId}, {"user_uuid", userId},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}, {"revision", 17},
            {"created_at", "2026-01-02T10:00:00.000001Z"}, {"updated_at", "2026-01-02T10:00:01.123456Z"},
            {"payload", QJsonObject{{"kind", "markdown"}, {"content", "**Draft**\n  preserved\n"}}}};
        const auto parsed = parseDraft(raw, projectId, userId, "\"17\"");
        QVERIFY(parsed);
        QCOMPARE(parsed->uuid, returnedId); QCOMPARE(parsed->project, projectId); QCOMPARE(parsed->user, userId);
        QCOMPARE(parsed->stream, streamId); QCOMPARE(parsed->topic, topicId);
        QCOMPARE(parsed->content, QString("**Draft**\n  preserved\n"));
        QCOMPARE(parsed->revision, 17); QCOMPARE(parsed->etag, QByteArray("\"17\""));
        QCOMPARE(parsed->createdAt, raw.value("created_at").toString());
        QCOMPARE(parsed->updatedAt, raw.value("updated_at").toString());
        QCOMPARE(parsed->updatedMicros, QDateTime::fromString("2026-01-02T10:00:01.123Z", Qt::ISODateWithMs)
            .toMSecsSinceEpoch() * 1000 + 456);
        const auto listItem = parseDraft(raw, projectId, userId);
        QVERIFY(listItem); QCOMPARE(listItem->etag, parsed->etag);
        QVERIFY(!parseDraft(raw, secondTopicId, userId));
        QVERIFY(!parseDraft(raw, projectId, secondTopicId));
    }

    void draftWireAdmission_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        for (const auto *field : {"uuid", "stream_uuid", "topic_uuid", "project_id", "user_uuid"}) {
            QTest::newRow(qPrintable(QString(field) + "-invalid")) << QString(field) << QJsonValue("invalid");
            QTest::newRow(qPrintable(QString(field) + "-absent")) << QString(field) << QJsonValue(QJsonValue::Undefined);
        }
        QTest::newRow("foreign-project") << QString("project_id") << QJsonValue(secondTopicId);
        QTest::newRow("foreign-user") << QString("user_uuid") << QJsonValue(secondTopicId);
        for (const auto *field : {"created_at", "updated_at"}) {
            QTest::newRow(qPrintable(QString(field) + "-invalid")) << QString(field) << QJsonValue("not-a-date");
            QTest::newRow(qPrintable(QString(field) + "-absent")) << QString(field) << QJsonValue(QJsonValue::Undefined);
        }
        QTest::newRow("revision-missing") << QString("revision") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("revision-zero") << QString("revision") << QJsonValue(0);
        QTest::newRow("revision-negative") << QString("revision") << QJsonValue(-1);
        QTest::newRow("revision-fraction") << QString("revision") << QJsonValue(1.5);
        QTest::newRow("revision-string") << QString("revision") << QJsonValue("1");
        QTest::newRow("revision-null") << QString("revision") << QJsonValue(QJsonValue::Null);
        QTest::newRow("revision-unsafe") << QString("revision") << QJsonValue(9007199254740992.0);
        QTest::newRow("payload-missing") << QString("payload") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("payload-array") << QString("payload") << QJsonValue(QJsonArray{});
        QTest::newRow("payload-ciphertext") << QString("payload")
            << QJsonValue(QJsonObject{{"kind", "e2ee"}, {"content", "opaque"}});
        QTest::newRow("content-number") << QString("payload")
            << QJsonValue(QJsonObject{{"kind", "markdown"}, {"content", 123}});
        QTest::newRow("content-empty") << QString("payload")
            << QJsonValue(QJsonObject{{"kind", "markdown"}, {"content", ""}});
        QTest::newRow("content-python-space") << QString("payload")
            << QJsonValue(QJsonObject{{"kind", "markdown"}, {"content", QString(QChar(0x85)) + '\n'}});
        QTest::newRow("content-too-long") << QString("payload")
            << QJsonValue(QJsonObject{{"kind", "markdown"}, {"content", QString(40001, 'a')}});
        QTest::newRow("content-unpaired") << QString("payload")
            << QJsonValue(QJsonObject{{"kind", "markdown"}, {"content", QString(QChar(0xd800))}});
    }
    void draftWireAdmission()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        QJsonObject raw{{"uuid", returnedId}, {"project_id", projectId}, {"user_uuid", userId},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}, {"revision", 1},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:01Z"},
            {"payload", QJsonObject{{"kind", "markdown"}, {"content", "Draft"}}}};
        raw[field] = value;
        QVERIFY(!parseDraft(raw, projectId, userId));
    }

    void draftRevisionHeaderAndUnicodeBoundaries_data()
    {
        QTest::addColumn<QByteArray>("etag"); QTest::addColumn<QString>("text"); QTest::addColumn<bool>("accepted");
        QTest::newRow("strong") << QByteArray("\"1\"") << QString("Draft") << true;
        QTest::newRow("collection-fallback") << QByteArray() << QString("Draft") << true;
        QTest::newRow("weak") << QByteArray("W/\"1\"") << QString("Draft") << false;
        QTest::newRow("other-revision") << QByteArray("\"2\"") << QString("Draft") << false;
        QTest::newRow("missing-quotes") << QByteArray("1") << QString("Draft") << false;
        QTest::newRow("leading-zero") << QByteArray("\"01\"") << QString("Draft") << false;
        QTest::newRow("unicode-limit") << QByteArray("\"1\"") << QString::fromUtf8("😀").repeated(40000) << true;
        QTest::newRow("unicode-over-limit") << QByteArray("\"1\"") << QString::fromUtf8("😀").repeated(40001) << false;
        QTest::newRow("bom-is-backend-content") << QByteArray("\"1\"") << QString(QChar(0xfeff)) << true;
    }
    void draftRevisionHeaderAndUnicodeBoundaries()
    {
        QFETCH(QByteArray, etag); QFETCH(QString, text); QFETCH(bool, accepted);
        const QJsonObject raw{{"uuid", returnedId}, {"project_id", projectId}, {"user_uuid", userId},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}, {"revision", 1},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:01Z"},
            {"payload", QJsonObject{{"kind", "markdown"}, {"content", text}}}};
        const auto parsed = parseDraft(raw, projectId, userId, etag);
        QCOMPARE(parsed.has_value(), accepted);
        if (parsed) QCOMPARE(parsed->content, text);
    }

    void wireDeviceLimitsCountCodePointsNotUtf16Units()
    {
        const auto nonBmp = QString::fromUtf8("😀");
        auto row = message(); auto payload = QJsonObject{{"kind", "key_request"}, {"key_uuid", returnedId},
            {"device_id", nonBmp.repeated(255)}, {"device_name", nonBmp.repeated(255)}, {"public_key", "opaque-public"}};
        row["payload"] = payload; const auto parsed = parseMessage(row, projectId);
        QVERIFY(parsed); QVERIFY(parsed->crypto); QCOMPARE(parsed->crypto->deviceId.size(), 510);
        payload["device_id"] = nonBmp.repeated(256); row["payload"] = payload; QVERIFY(!parseMessage(row, projectId));
        payload["device_id"] = "sender"; payload["device_name"] = nonBmp.repeated(256); row["payload"] = payload;
        QVERIFY(!parseMessage(row, projectId));
    }

    void legacyMetadataAndUnknownPayloadRemainNonAuthoritative()
    {
        auto raw = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Legacy"}, {"encryption", true}};
        const auto stream = parseStream(raw, projectId); QVERIFY(stream); QVERIFY(!stream->security);
        auto row = message(); row["payload"] = QJsonObject{{"kind", "future-service"}, {"content", "untrusted future bytes"}};
        const auto parsed = parseMessage(row, projectId); QVERIFY(parsed); QVERIFY(!parsed->crypto); QVERIFY(parsed->content.isEmpty());
    }

    void typedCryptoRejectsMalformedWireFields_data()
    {
        QTest::addColumn<QJsonObject>("payload");
        QTest::newRow("invalid-key") << QJsonObject{{"kind", "e2ee"}, {"key_uuid", "bad"}, {"content", "opaque"}};
        QTest::newRow("missing-content") << QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId}};
        QTest::newRow("oversized-e2ee") << QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId}, {"content", QString(213337, 'a')}};
        QTest::newRow("numeric-public") << QJsonObject{{"kind", "key_request"}, {"key_uuid", returnedId}, {"device_id", "device"},
            {"device_name", "name"}, {"public_key", 17}};
        QTest::newRow("oversized-public") << QJsonObject{{"kind", "key_request"}, {"key_uuid", returnedId}, {"device_id", "device"},
            {"device_name", "name"}, {"public_key", QString(2049, 'a')}};
        QTest::newRow("invalid-request-reference") << QJsonObject{{"kind", "key_reject"}, {"key_uuid", returnedId},
            {"request_message_uuid", "bad"}, {"sender_device_id", "sender"}, {"recipient_device_id", "recipient"}};
        QTest::newRow("invalid-device-type") << QJsonObject{{"kind", "key_reject"}, {"key_uuid", returnedId},
            {"request_message_uuid", messageId}, {"sender_device_id", 17}, {"recipient_device_id", "recipient"}};
    }
    void typedCryptoRejectsMalformedWireFields()
    {
        QFETCH(QJsonObject, payload); auto row = message(); row["payload"] = payload; QVERIFY(!parseMessage(row, projectId));
    }

    void streamCryptoRejectsMalformedAuthorityEvenWhenLegacyFieldsAreMissing_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("owner") << QString("owner") << QJsonValue("bad");
        QTest::newRow("viewer") << QString("user_uuid") << QJsonValue(17);
        QTest::newRow("generation") << QString("current_encryption_key_uuid") << QJsonValue("bad");
        QTest::newRow("provider") << QString("provider_uuid") << QJsonValue("bad");
        QTest::newRow("provider-identity") << QString("provider_identity") << QJsonValue(17);
        QTest::newRow("default-topic") << QString("default_topic_uuid") << QJsonValue("bad");
        QTest::newRow("source") << QString("source_name") << QJsonValue(17);
    }
    void streamCryptoRejectsMalformedAuthorityEvenWhenLegacyFieldsAreMissing()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        auto raw = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Partial"}, {"encryption", true}};
        raw[field] = value; QVERIFY(!parseStream(raw, projectId));
    }

    void typedCryptoMetadataRetainsAuthorityWithoutMakingItVisible()
    {
        QJsonObject raw{{"uuid", streamId}, {"project_id", projectId}, {"name", "Encrypted"},
            {"encryption", true}, {"owner", userId}, {"user_uuid", userId}, {"source_name", "native"},
            {"provider_uuid", QJsonValue::Null}, {"provider_identity", QJsonValue::Null},
            {"current_encryption_key_uuid", returnedId}, {"default_topic_uuid", topicId}};
        const auto stream = parseStream(raw, projectId); QVERIFY(stream); QVERIFY(stream->security);
        QCOMPARE(stream->security->owner, userId); QCOMPARE(stream->security->viewer, userId);
        QCOMPARE(stream->security->currentKey, returnedId); QCOMPARE(stream->security->defaultTopic, topicId);
        QCOMPARE(stream->security->source, QString("native")); QVERIFY(stream->security->provider.isEmpty());
        auto row = message(); row["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", secondTopicId}, {"content", "opaque-base64"}};
        const auto parsed = parseMessage(row, projectId); QVERIFY(parsed); QVERIFY(parsed->crypto);
        QCOMPARE(parsed->crypto->keyUuid, secondTopicId); // Historical A is retained under current B.
        QCOMPARE(parsed->crypto->encodedContent, QString("opaque-base64")); QVERIFY(parsed->content.isEmpty());
    }

    void typedCryptoPayloadRetainsEveryControlField_data()
    {
        QTest::addColumn<QJsonObject>("payload");
        QTest::newRow("request") << QJsonObject{{"kind", "key_request"}, {"key_uuid", returnedId},
            {"device_id", "device/string"}, {"device_name", "Synthetic device"}, {"public_key", "opaque-public"}};
        QTest::newRow("grant") << QJsonObject{{"kind", "key_grant"}, {"key_uuid", returnedId},
            {"request_message_uuid", messageId}, {"sender_device_id", "sender"}, {"recipient_device_id", "recipient"}, {"content", "opaque-grant"}};
        QTest::newRow("reject") << QJsonObject{{"kind", "key_reject"}, {"key_uuid", returnedId},
            {"request_message_uuid", messageId}, {"sender_device_id", "sender"}, {"recipient_device_id", "recipient"}};
        QTest::newRow("announcement") << QJsonObject{{"kind", "key_announced"}, {"key_uuid", returnedId}};
    }
    void typedCryptoPayloadRetainsEveryControlField()
    {
        QFETCH(QJsonObject, payload); auto row = message(); row["payload"] = payload;
        const auto parsed = parseMessage(row, projectId); QVERIFY(parsed); QVERIFY(parsed->crypto);
        QCOMPARE(parsed->crypto->keyUuid, returnedId); QVERIFY(parsed->content.isEmpty());
        QCOMPARE(parsed->crypto->deviceId, payload.value("device_id").toString());
        QCOMPARE(parsed->crypto->encodedPublicKey, payload.value("public_key").toString());
        QCOMPARE(parsed->crypto->requestMessage, payload.value("request_message_uuid").toString());
        QCOMPARE(parsed->crypto->senderDevice, payload.value("sender_device_id").toString());
        QCOMPARE(parsed->crypto->recipientDevice, payload.value("recipient_device_id").toString());
    }

    void privateStreamMetadataIsTypedAndLegacyOptional()
    {
        QJsonObject raw{{"uuid", streamId}, {"project_id", projectId}, {"name", "Direct"}, {"encryption", false}};
        QVERIFY(parseStream(raw, projectId)); QVERIFY(!parseStream(raw, projectId)->privateStream);
        raw["private"] = true; raw["direct_user_uuid"] = userId;
        QVERIFY(parseStream(raw, projectId)); QVERIFY(parseStream(raw, projectId)->privateStream);
        raw["private"] = false; QVERIFY(!parseStream(raw, projectId)->privateStream);
        for (const auto &bad : {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
            raw["private"] = bad; QVERIFY(!parseStream(raw, projectId));
        }
    }

    void pickerPresentationPreservesSourceOrderVariantsAndWireIdentity()
    {
        QFile metadata(":/workspace/data/emoji_picker.json");
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(metadata.readAll(), QCryptographicHash::Sha256).toHex(),
            QByteArray("7ebd24dbaaecb9723d578dda1b25e4ac0c56dd75eb81e60fcd93cc3ede06a552"));
        const auto choices = knownReactionEmojis();
        QCOMPARE(choices.size(), 3979);
        QMap<QString, QVariantMap> byName;
        int mapped = 0, eligible = 0, bases = 0;
        for (const auto &value : choices) {
            const auto row = value.toMap();
            const auto id = row.value("emojiName").toString();
            byName.insert(id, row);
            mapped += row.value("pickerOrder", -1).toInt() >= 0;
            eligible += row.value("pickerEligible").toBool();
            bases += row.value("pickerBase").toString() == id;
            QCOMPARE(canonicalReactionName(row.value("unicode").toString()), id);
            if (row.value("pickerOrder", -1).toInt() >= 0)
                QCOMPARE(canonicalReactionName(row.value("pickerUnicode").toString()), id);
        }
        QCOMPARE(mapped, 3773);
        QCOMPARE(eligible, 3770);
        QCOMPARE(bases, 1898);
        QCOMPARE(byName.value("grinning").value("pickerCategory").toString(), "smileys_people");
        QCOMPARE(byName.value("grinning").value("pickerOrder").toInt(), 0);
        QCOMPARE(byName.value("grinning_face_with_big_eyes").value("pickerOrder").toInt(), 1);
        QCOMPARE(byName.value("wave").value("pickerVariants").toList().size(), 5);
        QCOMPARE(byName.value("wave_tone1").value("pickerBase").toString(), "wave");
        QCOMPARE(byName.value("wave_tone1").value("pickerUnicode").toString(), QString::fromUtf8("👋🏻"));
        QCOMPARE(byName.value("handshake").value("pickerVariants").toList().size(), 25);
        QVERIFY(!byName.value("female").value("pickerEligible").toBool());
        QCOMPARE(byName.value("female").value("pickerCategory").toString(), "symbols");
        QCOMPARE(byName.value("heart").value("unicode").toString(), QString::fromUtf8("❤"));
        QCOMPARE(byName.value("heart").value("pickerUnicode").toString(), QString::fromUtf8("❤️"));
        QCOMPARE(reactionUnicode("heart"), QString::fromUtf8("❤️"));
        for (const auto &row : byName) {
            const auto base = row.value("pickerBase").toString();
            if (base.isEmpty()) continue;
            QVERIFY(byName.contains(base));
            const auto sourceBase = byName.value(base);
            QCOMPARE(row.value("pickerCategory"), sourceBase.value("pickerCategory"));
            QCOMPARE(row.value("pickerOrder"), sourceBase.value("pickerOrder"));
            for (const auto &variant : row.value("pickerVariants").toList()) {
                QVERIFY(byName.contains(variant.toString()));
                QCOMPARE(byName.value(variant.toString()).value("pickerBase").toString(), base);
            }
        }
    }

    void reactionCatalogUsesBackendCanonicalIdentity()
    {
        QFile catalog(":/workspace/data/unicode_emoji.json"); QVERIFY(catalog.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(catalog.readAll(), QCryptographicHash::Sha256).toHex(),
            QByteArray("c02c9b3ddad6a82e61eb290b86d9b803e00407eca69103908a376d548b308716"));
        QCOMPARE(knownReactionEmojis().size(), 3979);
        for (const auto &row : knownReactionEmojis()) {
            const auto choice = row.toMap();
            const auto canonical = choice.value("emojiName").toString();
            QCOMPARE(canonicalReactionName(choice.value("unicode").toString()), canonical);
            for (const auto &alias : choice.value("aliases").toList()) QCOMPARE(canonicalReactionName(alias.toString()), canonical);
        }
        QCOMPARE(canonicalReactionName(" :Thumbs-Up: "), "thumbs_up");
        QCOMPARE(canonicalReactionName(QString::fromUtf8("👍")), "thumbs_up");
        QCOMPARE(canonicalReactionName(QString::fromUtf8("❤️")), "heart");
        QCOMPARE(canonicalReactionName("-1"), "1");
        QCOMPARE(canonicalReactionName("SMILE"), "grinning_face_with_closed_eyes");
        QVERIFY(canonicalReactionName("unknown-provider-name").isEmpty());
        QCOMPARE(reactionUnicode("thumbs_up"), QString::fromUtf8("👍"));
        QVERIFY(reactionUnicode("<img src=secret>").isEmpty());
        QVERIFY(validReactionName("future-provider-emoji"));
        QVERIFY(!validReactionName(QString(QChar(0xfeff))));
        QVERIFY(validReactionName(QString(QChar(0x85))));
        QVERIFY(!validReactionName(QString(129, 'a')));
    }

    void validatesReactionSnapshotsWithoutGuessingOwnership()
    {
        auto json = message();
        json["reactions"] = QJsonObject{{"thumbs_up", 3}, {"future-provider-emoji", 1}};
        json["reaction_users"] = QJsonObject{{"future-provider-emoji", QJsonArray{userId}}};
        auto parsed = parseMessage(json, projectId);
        QVERIFY(parsed); QVERIFY(parsed->reactionsKnown);
        QCOMPARE(parsed->reactions.value("thumbs_up"), 3);
        QVERIFY(!parsed->reactionUsers.contains("thumbs_up"));
        QCOMPARE(parsed->reactionUsers.value("future-provider-emoji"), QStringList{userId});
        for (const auto &bad : {QJsonValue(-1), QJsonValue(0.5), QJsonValue("2")}) {
            json["reactions"] = QJsonObject{{"thumbs_up", bad}};
            QVERIFY(!parseMessage(json, projectId));
        }
        json["reactions"] = QJsonObject{{"thumbs_up", 1}};
        json["reaction_users"] = QJsonObject{{"thumbs_up", QJsonArray{userId, userId}}};
        QVERIFY(!parseMessage(json, projectId));
        json["reaction_users"] = QJsonObject{{"thumbs_up", QJsonArray{"not-a-uuid"}}};
        QVERIFY(!parseMessage(json, projectId));
    }

    void countersKeepSourceFieldsAndRejectCorruptValues()
    {
        auto stream = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Channel"}, {"encryption", false},
            {"unread_count", 7}, {"active_unread_count", 5}, {"passive_unread_count", 2}, {"last_message_uuid", returnedId}};
        const auto parsed = parseStream(stream, projectId);
        QVERIFY(parsed); QCOMPARE(parsed->unread, 7); QCOMPARE(parsed->activeUnread, 5); QCOMPARE(parsed->passiveUnread, 2);
        QCOMPARE(parsed->lastMessage, returnedId);
        const auto topic = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Topic"},
            {"unread_count", 7}, {"active_unread_count", 5}, {"passive_unread_count", 2}, {"last_message_uuid", QJsonValue::Null}};
        const auto parsedTopic = parseTopic(topic, projectId, streamId);
        QVERIFY(parsedTopic); QCOMPARE(parsedTopic->activeUnread, 5); QVERIFY(parsedTopic->lastMessage.isEmpty());
        for (const auto &bad : {QJsonValue(-1), QJsonValue(1.5), QJsonValue("0"), QJsonValue(9007199254740992.0)}) {
            auto invalid = stream; invalid["unread_count"] = bad; QVERIFY(!parseStream(invalid, projectId));
            auto invalidTopic = topic; invalidTopic["passive_unread_count"] = bad; QVERIFY(!parseTopic(invalidTopic, projectId, streamId));
        }
        stream["last_message_uuid"] = "not-a-uuid"; QVERIFY(!parseStream(stream, projectId));
    }

    void rejectsCrossProjectAndInvalidHistory()
    {
        auto json = message();
        QVERIFY(parseMessage(json, projectId));
        QVERIFY(!parseMessage(json, userId));
        json.insert("topic_uuid", "not-a-uuid");
        QVERIFY(!parseMessage(json, projectId));
        json = message();
        json.insert("created_at", "not-a-date");
        QVERIFY(!parseMessage(json, projectId));
    }

    void ciphertextIsNeverExposedAsText()
    {
        auto json = message();
        json.insert("payload", QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId}, {"content", "opaque-ciphertext"}});
        const auto parsed = parseMessage(json, projectId);
        QVERIFY(parsed);
        QCOMPARE(parsed->kind, "e2ee");
        QVERIFY(parsed->content.isEmpty());
    }

    void overlappingPagesReplaceByReturnedUuidAndSort()
    {
        auto one = *parseMessage(message("before", messageId), projectId);
        auto changed = *parseMessage(message("after", messageId), projectId);
        auto two = *parseMessage(message("second", returnedId), projectId);
        const auto result = mergeMessages({two, one}, {changed, two});
        QCOMPARE(result.size(), 2);
        QCOMPARE(result.at(0).uuid, messageId);
        QCOMPARE(result.at(0).content, "after");
        QCOMPARE(result.at(1).uuid, returnedId);
    }

    void topicOrderingMetadataRejectsUntrustedTypesAndModes()
    {
        QJsonObject topic{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Topic"}};
        for (const auto &invalid : {QJsonValue(4), QJsonValue("not-a-date")}) {
            auto copy = topic; copy["updated_at"] = invalid; QVERIFY(!parseTopic(copy, projectId, streamId));
        }
        for (const auto &invalid : {QJsonValue(4), QJsonValue("unknown")}) {
            auto copy = topic; copy["notification_mode"] = invalid; QVERIFY(!parseTopic(copy, projectId, streamId));
        }
    }

    void topicBelongsToExpectedStream()
    {
        const QJsonObject topic{{"uuid", topicId}, {"project_id", projectId},
                               {"stream_uuid", streamId}, {"name", "Topic"}};
        QVERIFY(parseTopic(topic, projectId, streamId));
        QVERIFY(!parseTopic(topic, projectId, secondTopicId));
    }

    void preservesSubMillisecondMessageOrder()
    {
        auto earlierJson = message("earlier", returnedId);
        auto laterJson = message("later", messageId);
        earlierJson.insert("created_at", "2026-01-02T10:00:00.000100Z");
        laterJson.insert("created_at", "2026-01-02T10:00:00.000200Z");
        const auto earlier = *parseMessage(earlierJson, projectId);
        const auto later = *parseMessage(laterJson, projectId);
        const auto result = mergeMessages({later}, {earlier});
        QCOMPARE(result.first().uuid, returnedId);
    }
};

QTEST_GUILESS_MAIN(DomainTest)
#include "domain-test.moc"
