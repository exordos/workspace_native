#include "workspace-reference.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace workspace;

namespace {
const QString User = "11111111-1111-4111-8111-111111111111";
const QString Message = "22222222-2222-4222-8222-222222222222";
const QString Stream = "33333333-3333-4333-8333-333333333333";
const QString Topic = "44444444-4444-4444-8444-444444444444";
}

class WorkspaceReferenceTest : public QObject {
    Q_OBJECT
private slots:
    void canonicalAndLegacyTopics();
    void quoteTextHasDecodeURIComponentSemantics();
    void forwardCrossClientVectors();
    void malformedReferences_data();
    void malformedReferences();
    void fileQueryHasUrlSearchParamsSemantics();
    void timeCalendarAndZones();
    void secretMetadataNeverRetainsPayload();
    void mentionIdentityAndEscaping();
    void externalLinksNeverGainCredentialsOrLocalAccess();
};

void WorkspaceReferenceTest::canonicalAndLegacyTopics()
{
    for (const auto &target : {"urn:user:" + User, "urn:message:" + Message, "urn:stream:" + Stream,
             "urn:topic:" + Topic, "urn:topic:" + Stream + ':' + Topic}) {
        const auto result = parseWorkspaceReference("  " + target + "  ");
        QVERIFY(result);
        QVERIFY(!result->id.isEmpty());
    }
    const auto current = parseWorkspaceReference("urn:topic:" + Topic);
    QVERIFY(current); QCOMPARE(current->id, Topic); QVERIFY(current->streamUuid.isEmpty());
    const auto legacy = parseWorkspaceReference("urn:topic:" + Stream + ':' + Topic);
    QVERIFY(legacy); QCOMPARE(legacy->id, Topic); QCOMPARE(legacy->streamUuid, Stream);
    QCOMPARE(legacy->toVariantMap().value("stream").toString(), Stream);
    QVERIFY(parseWorkspaceReference(QChar(0xfeff) + QString("urn:topic:") + Topic + QChar(0xfeff)));
}

void WorkspaceReferenceTest::quoteTextHasDecodeURIComponentSemantics()
{
    const auto text = QString("Юникод\nC++ & # ? % )");
    const auto urn = buildWorkspaceQuoteUrn(Message, text);
    QCOMPARE(urn, "urn:quote:" + Message + "?text=%D0%AE%D0%BD%D0%B8%D0%BA%D0%BE%D0%B4%0AC%2B%2B%20%26%20%23%20%3F%20%25%20%29");
    const auto parsed = parseWorkspaceReference(urn);
    QVERIFY(parsed); QVERIFY(parsed->text); QCOMPARE(*parsed->text, text);
    const auto literalPlus = parseWorkspaceReference("urn:quote:" + Message + "?text=C++");
    QVERIFY(literalPlus && literalPlus->text); QCOMPARE(*literalPlus->text, "C++");
    const auto empty = parseWorkspaceReference("urn:quote:" + Message + "?text=");
    QVERIFY(empty); QVERIFY(!empty->text); QVERIFY(!empty->toVariantMap().contains("text"));
    const auto bom = parseWorkspaceReference("urn:quote:" + Message + "?text=%EF%BB%BFx");
    QVERIFY(bom && bom->text); QCOMPARE(*bom->text, QString(QChar(0xfeff)) + 'x');
}

void WorkspaceReferenceTest::forwardCrossClientVectors()
{
    const auto vector = "urn:forward:" + Message + "?snapshot=0J_RgNC40LLQtdGCIOKckw";
    QCOMPARE(buildWorkspaceForwardUrn(Message, "Привет ✓"), vector);
    const auto parsed = parseWorkspaceReference(vector);
    QVERIFY(parsed); QCOMPARE(parsed->snapshotMarkdown, "Привет ✓");
    WorkspaceForwardOptions options;
    options.sourceLabel = "Engineering · Общий чат";
    options.sourceCreatedAt = "2026-09-11T16:47:00+03:00";
    options.directSource = true;
    options.plainSnapshot = true;
    const auto urn = buildWorkspaceForwardUrn(Message, "Привет ✓", options);
    QCOMPARE(urn, vector + "&format=plain&source=RW5naW5lZXJpbmcgwrcg0J7QsdGJ0LjQuSDRh9Cw0YI&source_kind=direct&created_at=2026-09-11T16%3A47%3A00%2B03%3A00");
    const auto metadata = parseWorkspaceReference(urn);
    QVERIFY(metadata); QCOMPARE(metadata->sourceLabel, *options.sourceLabel);
    QCOMPARE(metadata->sourceCreatedAt, *options.sourceCreatedAt);
    QVERIFY(metadata->plainSnapshot); QVERIFY(metadata->directSource);
    const auto empty = parseWorkspaceReference("urn:forward:" + Message + "?snapshot=");
    QVERIFY(empty); QVERIFY(empty->snapshotMarkdown.isEmpty());
}

void WorkspaceReferenceTest::malformedReferences_data()
{
    QTest::addColumn<QString>("target");
    const QStringList invalid{
        "urn:stream:not-a-uuid", "urn:user:" + User + "?silent=true", "urn:topic:" + Topic + ":extra",
        "urn:quote:" + Message + "?other=x", "urn:quote:" + Message + "?text=x&text=y",
        "urn:quote:" + Message + "?text=%E0%A4%A", "urn:quote:" + Message + "?text=%ED%A0%80",
        "urn:quote:" + Message + "?text=%FF", "urn:quote:" + Message + "?text=%",
        "urn:forward:" + Message, "urn:forward:" + Message + "?snapshot", "urn:forward:" + Message + "?snapshot=%%%",
        "urn:forward:" + Message + "?snapshot=Zm9v=", "urn:forward:" + Message + "?snapshot=AB",
        "urn:forward:" + Message + "?snapshot=_w", "urn:forward:" + Message + "?snapshot=Zm9v&snapshot=YmFy",
        "urn:forward:" + Message + "?snapshot=77u_Zm9v",
        "urn:forward:" + Message + "?snapshot=Zm9v&unknown=x", "urn:forward:" + Message + "?snapshot=Zm9v&format=markdown",
        "urn:forward:" + Message + "?snapshot=Zm9v&source_kind=direct", "urn:forward:" + Message + "?snapshot=Zm9v&source=eA",
        "urn:forward:" + Message + "?snapshot=Zm9v&created_at=2026-09-11T16%3A47%3A00Z",
        "urn:forward:" + Message + "?snapshot=Zm9v&source=eA&created_at=not-a-date",
        "urn:forward:" + Message + "?snapshot=Zm9v&source=eA&source_kind=channel&created_at=2026-09-11T16%3A47%3A00Z",
        "urn:forward:" + Message + "?snapshot=Zm9v&source=eA&created_at=%FF",
        "urn:quote:" + Message + "?text=before" + QString(QChar(0x2028)) + "after",
        "urn:forward:" + Message + "?snapshot=Zm9v" + QString(QChar(0x2029)) + "&format=plain",
        "urn:time:2026-01-01T12:00", "file:///etc/passwd", "javascript:alert(1)", "data:text/html,evil"
    };
    for (qsizetype i = 0; i < invalid.size(); ++i) QTest::newRow(qPrintable(QString::number(i))) << invalid[i];
}

void WorkspaceReferenceTest::malformedReferences()
{
    QFETCH(QString, target);
    QVERIFY(!parseWorkspaceReference(target));
}

void WorkspaceReferenceTest::fileQueryHasUrlSearchParamsSemantics()
{
    const auto parsed = parseWorkspaceReference("urn:image:" + Message
        + "?name=C%2B%2B+diagram.png&content_type=image%2Fpng&w=480&h=270&size=0&name=ignored", "fallback");
    QVERIFY(parsed); QCOMPARE(parsed->label, "C++ diagram.png"); QCOMPARE(parsed->contentType, "image/png");
    QCOMPARE(*parsed->width, 480); QCOMPARE(*parsed->height, 270); QCOMPARE(*parsed->sizeBytes, 0);
    const auto invalidDimensions = parseWorkspaceReference("urn:video:" + Message + "?name=+&w=0&h=-1&size=9007199254740992", " label ");
    QVERIFY(invalidDimensions); QCOMPARE(invalidDimensions->label, "label");
    QVERIFY(!invalidDimensions->width); QVERIFY(!invalidDimensions->height); QVERIFY(!invalidDimensions->sizeBytes);
    const auto blankName = parseWorkspaceReference("urn:file:" + Message + "?name=%EF%BB%BF", "label");
    QVERIFY(blankName); QCOMPARE(blankName->label, "label");
}

void WorkspaceReferenceTest::timeCalendarAndZones()
{
    QCOMPARE(parseWorkspaceMessageTime("2026-09-11T16:47+03:00"), "2026-09-11T13:47:00Z");
    QCOMPARE(parseWorkspaceMessageTime("2024-02-29T23:59:01.12-02:30"), "2024-03-01T02:29:01.120Z");
    QCOMPARE(parseWorkspaceMessageTime("0001-01-01T00:00+23:59"), "0000-12-31T00:01:00Z");
    QCOMPARE(parseWorkspaceMessageTime("9999-12-31T23:59-23:59"), "+010000-01-01T23:58:00Z");
    for (const auto *invalid : {"2026-02-29T12:00Z", "2026-04-31T12:00Z", "0000-01-01T00:00Z", "2026-01-01T24:00Z", "2026-01-01T23:59:60Z", "2026-01-01T12:00+24:00", "2026-01-01T12:00z", "2026-01-01T12:00:00.1234Z"})
        QVERIFY(parseWorkspaceMessageTime(QLatin1String(invalid)).isEmpty());
    const auto reference = parseWorkspaceReference("urn:time:2026-09-11T16:47+03:00");
    QVERIFY(reference); QCOMPARE(reference->utc, "2026-09-11T13:47:00Z");
}

void WorkspaceReferenceTest::secretMetadataNeverRetainsPayload()
{
    const auto valid = "urn:secret:v1:" + Stream + ':' + User + ":AAECAwQFBgc";
    for (const auto &urn : {valid, QString("urn:secret:v99:untrusted"), QString("URN:SECRET:invalid"), valid + '=', QChar(0xfeff) + QString("urn:secret:v99:untrusted")}) {
        QVERIFY(isWorkspaceSecretReference(urn));
        const auto parsed = parseWorkspaceReference(urn, "DO NOT DISPLAY");
        QVERIFY(parsed); QCOMPARE(parsed->kind, WorkspaceReferenceKind::Secret);
        QVERIFY(parsed->target.isEmpty()); QVERIFY(parsed->label.isEmpty()); QVERIFY(parsed->snapshotMarkdown.isEmpty());
        const auto map = parsed->toVariantMap();
        QCOMPARE(map.value("kind").toString(), "secret");
        QVERIFY(!map.contains("target")); QVERIFY(!map.contains("text")); QVERIFY(!map.contains("envelope"));
        const auto serialized = QJsonDocument(QJsonObject::fromVariantMap(map)).toJson();
        QVERIFY(!serialized.contains("AAECAwQFBgc")); QVERIFY(!serialized.contains("DO NOT DISPLAY"));
        QCOMPARE(parsed->secretValid, urn == valid);
    }
    for (const auto &envelope : {QString(), QString("AB"), QString("A"), QString(40001, 'A')}) {
        const auto parsed = parseWorkspaceReference("urn:secret:v1:" + Stream + ':' + User + ':' + envelope);
        QVERIFY(parsed); QVERIFY(!parsed->secretValid);
    }
}

void WorkspaceReferenceTest::mentionIdentityAndEscaping()
{
    QCOMPARE(buildWorkspaceUserMention(" Alice [ops]* ", User), "[Alice \\[ops\\]\\*](urn:user:" + User + ')');
    const auto mention = parseWorkspaceReference("urn:user:" + User, "Different display name", true);
    QVERIFY(mention); QCOMPARE(mention->id, User); QVERIFY(mention->silent);
    QCOMPARE(mention->toVariantMap().value("silent").toBool(), true);
    const auto prefixed = parseWorkspaceReference("urn:user:" + User, "@@Alice   Ops");
    QVERIFY(prefixed); QCOMPARE(prefixed->label, "Alice Ops");
    QVERIFY(buildWorkspaceUserMention("Alice", "not-a-uuid").isEmpty());
    QCOMPARE(buildWorkspaceQuoteReference(" Alice ", Message, "C++"), "[Alice](urn:quote:" + Message + "?text=C%2B%2B)");
    QVERIFY(buildWorkspaceQuoteReference(" ", Message).isEmpty());
}

void WorkspaceReferenceTest::externalLinksNeverGainCredentialsOrLocalAccess()
{
    const auto parsed = parseWorkspaceReference("urn:url:https://example.invalid/a?q=C%2B%2B#section");
    QVERIFY(parsed); QCOMPARE(parsed->kind, WorkspaceReferenceKind::External);
    QCOMPARE(parsed->url, "https://example.invalid/a?q=C%2B%2B#section");
    for (const auto *target : {"https://user:password@example.invalid", "urn:url:file:///etc/passwd", "urn:url:javascript:alert", "urn:url:https://example.invalid/a b", "urn:url:urn:url:https://example.invalid", "//example.invalid/path", "qrc:/private", "https://example.invalid/\n"})
        QVERIFY(!parseWorkspaceReference(QLatin1String(target)));
    QVERIFY(!parseWorkspaceReference(QString(262145, 'x')));
    QVERIFY(!parseWorkspaceReference("urn:url:https://example.invalid/a" + QString(QChar(0xfeff)) + 'b'));
}

QTEST_GUILESS_MAIN(WorkspaceReferenceTest)
#include "workspace-reference-test.moc"
