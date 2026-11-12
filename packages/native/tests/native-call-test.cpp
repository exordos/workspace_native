#include "call/jitsi-call.h"

#include <QString>
#include <QStringList>
#include <QTest>

using namespace workspace;

class NativeCallTest final : public QObject {
    Q_OBJECT
private slots:
    // --- Meeting URL model (workspace_ui 61dacef7 reference contract) ---

    void workspaceRoomNameMatchesReferenceContract()
    {
        // workspace-jitsi-call.lib.test.ts: fixed nowMs 123 golden room name.
        QCOMPARE(buildWorkspaceJitsiRoomName(QStringLiteral("org-a"), QStringLiteral("project-a"),
                     QStringLiteral("75309057-419c-4b12-a7c1-3932429ec4a6"),
                     QStringLiteral("4ec0b996-b778-45f8-8ef4-ef863be0c047"), 123),
            QStringLiteral("workspace-org-a-project-a-75309057-419c-4b12-a7c1-3932429ec4a6"
                           "-4ec0b996-b778-45f8-8ef4-ef863be0c047-123"));
    }

    void meetingUrlUsesWorkspaceServerSettingsHost()
    {
        // workspace-jitsi-call.lib.test.ts golden URL.
        QCOMPARE(buildJitsiMeetingUrl(buildWorkspaceJitsiRoomName(QStringLiteral("org-a"),
                     QStringLiteral("project-a"), QStringLiteral("stream-a"),
                     QStringLiteral("topic-a"), 123),
                     QStringLiteral("https://meet.workspace.example.com")),
            QStringLiteral("https://meet.workspace.example.com/workspace-org-a-project-a-stream-a-topic-a-123"));
    }

    void roomSegmentsNormalizeAsReference_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("uppercase") << QStringLiteral("Acme-ORG") << QStringLiteral("acme-org");
        QTest::newRow("spaces-and-slash") << QStringLiteral(" Or g/ ") << QStringLiteral("or-g");
        QTest::newRow("underscores") << QStringLiteral("a__b") << QStringLiteral("a-b");
        QTest::newRow("symbol-run-collapses") << QStringLiteral("a!!@#b") << QStringLiteral("a-b");
        QTest::newRow("edge-dashes-stripped") << QStringLiteral("-x-") << QStringLiteral("x");
        QTest::newRow("only-symbols-to-room") << QStringLiteral("  ") << QStringLiteral("room");
        QTest::newRow("empty-to-room") << QStringLiteral("") << QStringLiteral("room");
        QTest::newRow("digits-hex") << QStringLiteral("ABCDEF-0123") << QStringLiteral("abcdef-0123");
    }
    void roomSegmentsNormalizeAsReference()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(normalizeRoomSegment(input), expected);
    }

    void roomNameStaysSixLogicalSegments()
    {
        const auto name = buildWorkspaceJitsiRoomName(QStringLiteral("https://Acme Example!!"),
            QStringLiteral("My Project"), QStringLiteral("STREAM-1"), QStringLiteral("_t2_"), 9);
        QCOMPARE(name, QStringLiteral("workspace-https-acme-example-my-project-stream-1-t2-9"));
    }

    void meetingUrlPercentEncodesRoom()
    {
        QCOMPARE(buildJitsiMeetingUrl(QStringLiteral("a b/c#"),
                     QStringLiteral("https://meet.example.com")),
            QStringLiteral("https://meet.example.com/a%20b%2Fc%23"));
    }

    void meetOriginNormalizationIsHttpOnlyOrigin_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("lowercases-scheme-host-trims-slash")
            << QStringLiteral("https://Meet.Example.com/") << QStringLiteral("https://meet.example.com");
        QTest::newRow("drops-path-query-fragment")
            << QStringLiteral("https://meet.example.com/sub/path?q=1#f") << QStringLiteral("https://meet.example.com");
        QTest::newRow("keeps-port")
            << QStringLiteral("http://meet.example.com:8443/x") << QStringLiteral("http://meet.example.com:8443");
        QTest::newRow("surrounding-space")
            << QStringLiteral("  https://a.b  ") << QStringLiteral("https://a.b");
        QTest::newRow("non-http-scheme") << QStringLiteral("ftp://meet.example.com") << QString();
        QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)") << QString();
        QTest::newRow("no-scheme") << QStringLiteral("meet.example.com") << QString();
        QTest::newRow("plain-text") << QStringLiteral("not a url") << QString();
        QTest::newRow("empty") << QStringLiteral("") << QString();
    }
    void meetOriginNormalizationIsHttpOnlyOrigin()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(normalizeMeetOrigin(input), expected);
    }

    void effectiveMeetBasePrefersServerSettingsOverBuildDefault()
    {
        QCOMPARE(effectiveMeetBase(QStringLiteral("https://meet.example.com/")),
            QStringLiteral("https://meet.example.com"));
        QCOMPARE(effectiveMeetBase(QStringLiteral("ftp://bad")),
            QStringLiteral("https://meet.jit.si"));
        QCOMPARE(effectiveMeetBase(QString()), QStringLiteral("https://meet.jit.si"));
    }

    void parseJitsiUrlAcceptsConfiguredHostAndPublicMeet_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("meetUrl");
        QTest::addColumn<bool>("accepted");
        QTest::addColumn<QString>("roomName");
        QTest::newRow("configured-host")
            << QStringLiteral("https://meet.example.com/Room%20Name")
            << QStringLiteral("https://meet.example.com") << true << QStringLiteral("Room Name");
        QTest::newRow("public-host-without-settings")
            << QStringLiteral("https://meet.jit.si/RoomA") << QString() << true << QStringLiteral("RoomA");
        QTest::newRow("public-host-still-accepted-with-settings")
            << QStringLiteral("https://meet.jit.si/RoomA")
            << QStringLiteral("https://meet.example.com") << true << QStringLiteral("RoomA");
        QTest::newRow("http-scheme-accepted")
            << QStringLiteral("http://meet.example.com/room-1")
            << QStringLiteral("https://meet.example.com") << true << QStringLiteral("room-1");
        QTest::newRow("other-host-rejected")
            << QStringLiteral("https://evil.example/room")
            << QStringLiteral("https://meet.example.com") << false << QString();
        QTest::newRow("no-path-rejected")
            << QStringLiteral("https://meet.example.com/")
            << QStringLiteral("https://meet.example.com") << false << QString();
    }
    void parseJitsiUrlAcceptsConfiguredHostAndPublicMeet()
    {
        QFETCH(QString, url);
        QFETCH(QString, meetUrl);
        QFETCH(bool, accepted);
        QFETCH(QString, roomName);
        const auto parsed = parseJitsiUrl(url, meetUrl);
        QCOMPARE(!parsed.isEmpty(), accepted);
        if (accepted)
            QCOMPARE(parsed.value(QStringLiteral("roomName")).toString(), roomName);
    }

    void jitsiUrlExtractionFollowsReferenceScan()
    {
        const QString meet = QStringLiteral("https://meet.example.com");
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("https://meet.example.com/room-1"), meet),
            QStringLiteral("https://meet.example.com/room-1"));
        QCOMPARE(getJitsiMeetingUrl(meet, meet), QString());
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("Join https://meet.example.com/r?x=1 now"), meet),
            QStringLiteral("https://meet.example.com/r?x=1"));
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("https://evil.example/r"), meet), QString());
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("see HTTPS://MEET.JIT.SI/abc here"), QString()),
            QStringLiteral("HTTPS://MEET.JIT.SI/abc"));
    }

    void callKeyNormalizesUrlAndKeepsOwnerSeparation_data()
    {
        QTest::addColumn<QString>("ownerKey");
        QTest::addColumn<QString>("meetingUrl");
        QTest::addColumn<QString>("expected");
        QTest::newRow("scheme-host-case-and-trailing-slash")
            << QStringLiteral("owner-a")
            << QStringLiteral("https://MEET.workspace.example.com/workspace-room-same/")
            << QStringLiteral("owner-a:https://meet.workspace.example.com/workspace-room-same");
        QTest::newRow("no-owner")
            << QString() << QStringLiteral("https://meet.example.com/r")
            << QStringLiteral(":https://meet.example.com/r");
        QTest::newRow("unparseable-falls-back-to-trimmed")
            << QStringLiteral("owner-a") << QStringLiteral(" weird url/ ")
            << QStringLiteral("owner-a:weird url");
    }
    void callKeyNormalizesUrlAndKeepsOwnerSeparation()
    {
        QFETCH(QString, ownerKey);
        QFETCH(QString, meetingUrl);
        QFETCH(QString, expected);
        QCOMPARE(createJitsiCallKey(ownerKey, meetingUrl), expected);
    }

    void roomDisplayNameTurnsSeparatorsIntoSpaces()
    {
        const QString meet = QStringLiteral("https://meet.example.com");
        QCOMPARE(formatRoomDisplayName(
                     QStringLiteral("https://meet.example.com/workspace-org_a-project-1"), meet),
            QStringLiteral("workspace org a project 1"));
        QCOMPARE(formatRoomDisplayName(QStringLiteral("https://meet.example.com/"), meet), QString());
        // A host outside the effective Workspace host (and not meet.jit.si)
        // is not a Jitsi room at all: no display name, the card would not
        // render either.
        QCOMPARE(formatRoomDisplayName(
                     QStringLiteral("https://other.example/workspace-a_b")), QString());
    }
};

QTEST_MAIN(NativeCallTest)
#include "native-call-test.moc"
