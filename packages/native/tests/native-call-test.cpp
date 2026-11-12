#include "call/jitsi-call.h"
#include "call/native-call-store.h"

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
        QTest::newRow("ipv6-brackets-preserved")
            << QStringLiteral("https://[2001:db8::1]/meet") << QStringLiteral("https://[2001:db8::1]");
        QTest::newRow("ipv6-brackets-with-port")
            << QStringLiteral("http://[2001:db8::1]:8443/x") << QStringLiteral("http://[2001:db8::1]:8443");
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

    void prefixMeetingUrlStopsAtFirstWhitespace()
    {
        // A body that merely begins with a meeting URL keeps its prose: only
        // the URL token is the meeting URL, the card must not swallow the rest
        // and joinCall must not receive "url join now" as a URL.
        const QString meet = QStringLiteral("https://meet.example.com");
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("https://meet.example.com/room-1 hop on when free"), meet),
            QStringLiteral("https://meet.example.com/room-1"));
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("https://meet.example.com/room-1\nsee you there"), meet),
            QStringLiteral("https://meet.example.com/room-1"));
        // A pure meeting-URL body is still returned whole (reference shortcut).
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("https://meet.example.com/room-1?x=1#f"), meet),
            QStringLiteral("https://meet.example.com/room-1?x=1#f"));
    }

    void embeddedMeetingUrlKeepsConfiguredPort()
    {
        // meet_url may carry a non-default port; an embedded link then reads
        // "host:port/room" and the scan must match the configured port.
        const QString meetPort = QStringLiteral("https://meet.example.com:8443");
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("Join https://meet.example.com:8443/r?x=1 now"), meetPort),
            QStringLiteral("https://meet.example.com:8443/r?x=1"));
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("https://meet.example.com:8443/room-1 join now"), meetPort),
            QStringLiteral("https://meet.example.com:8443/room-1"));
        // A different port is a different origin: not the configured host.
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("see https://meet.example.com:9999/r now"), meetPort),
            QString());
        // The no-port configuration keeps matching host-only links.
        const QString meet = QStringLiteral("https://meet.example.com");
        QCOMPARE(getJitsiMeetingUrl(QStringLiteral("Join https://meet.example.com/r now"), meet),
            QStringLiteral("https://meet.example.com/r"));
    }

    void ipv6MeetHostKeepsBracketsInOriginAndMeetingUrl()
    {
        // QUrl::host() reports an IPv6 address without its required brackets;
        // the serialized origin must put them back or the origin is invalid.
        QCOMPARE(effectiveMeetBase(QStringLiteral("https://[2001:db8::1]:8443/")),
            QStringLiteral("https://[2001:db8::1]:8443"));
        QCOMPARE(buildJitsiMeetingUrl(QStringLiteral("room-1"),
                     QStringLiteral("https://[2001:db8::1]:8443")),
            QStringLiteral("https://[2001:db8::1]:8443/room-1"));
        // A meeting URL on the IPv6 host parses back on the configured host.
        QCOMPARE(parseJitsiUrl(QStringLiteral("https://[2001:db8::1]:8443/room-1"),
                     QStringLiteral("https://[2001:db8::1]:8443"))
                     .value(QStringLiteral("roomName")).toString(),
            QStringLiteral("room-1"));
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

    // --- Single active call store (jitsi-call.model.ts mirror) ---

    void openFirstCallUsesPayloadAndClearsInvite()
    {
        JitsiCallStore store;
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 10);
        invite.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-10"));
        invite.insert(QStringLiteral("callerName"), QStringLiteral("Slon"));
        store.ingestIncomingInvite(invite);
        QCOMPARE(store.incomingInvite().value(QStringLiteral("messageId")).toInt(), 10);

        QVariantMap payload;
        payload.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-10"));
        payload.insert(QStringLiteral("locationName"), QStringLiteral("Slon"));
        payload.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-a"));
        payload.insert(QStringLiteral("meetUrl"), QStringLiteral("https://meet.workspace.example.com"));
        payload.insert(QStringLiteral("displayName"), QStringLiteral("Current User"));
        const auto result = store.requestOpenCall(payload);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        const auto active = store.activeCall();
        QCOMPARE(active.value(QStringLiteral("callKey")).toString(),
            QStringLiteral("owner-a:https://meet.workspace.example.com/workspace-room-10"));
        QCOMPARE(active.value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-10"));
        QCOMPARE(active.value(QStringLiteral("ownerKey")).toString(), QStringLiteral("owner-a"));
        QCOMPARE(active.value(QStringLiteral("meetUrl")).toString(),
            QStringLiteral("https://meet.workspace.example.com"));
        QCOMPARE(active.value(QStringLiteral("displayName")).toString(), QStringLiteral("Current User"));
        QCOMPARE(active.value(QStringLiteral("startWithVideoMuted")).toBool(), true);
        QVERIFY(active.value(QStringLiteral("startedAtMs")).toLongLong() > 0);
        QVERIFY(store.incomingInvite().isEmpty());
    }

    void sameCallPreservesOriginalActiveCall()
    {
        // jitsi-call.model.test.ts: same callKey (scheme/host case + trailing
        // slash tolerant) keeps the first active call object intact.
        JitsiCallStore store;
        QVariantMap first;
        first.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://MEET.workspace.example.com/workspace-room-same/"));
        first.insert(QStringLiteral("locationName"), QStringLiteral("First label"));
        first.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-a"));
        first.insert(QStringLiteral("displayName"), QStringLiteral("First User"));
        first.insert(QStringLiteral("startWithVideoMuted"), false);
        QCOMPARE(store.requestOpenCall(first).value(QStringLiteral("status")).toString(),
            QStringLiteral("opened"));
        const auto startedAt = store.activeCall().value(QStringLiteral("startedAtMs"));

        QVariantMap second;
        second.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-same"));
        second.insert(QStringLiteral("locationName"), QStringLiteral("Second label"));
        second.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-a"));
        second.insert(QStringLiteral("displayName"), QStringLiteral("Second User"));
        second.insert(QStringLiteral("startWithVideoMuted"), true);
        const auto result = store.requestOpenCall(second);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("same"));
        QCOMPARE(store.activeCall().value(QStringLiteral("locationName")).toString(),
            QStringLiteral("First label"));
        QCOMPARE(store.activeCall().value(QStringLiteral("displayName")).toString(),
            QStringLiteral("First User"));
        QCOMPARE(store.activeCall().value(QStringLiteral("startWithVideoMuted")).toBool(), false);
        QCOMPARE(store.activeCall().value(QStringLiteral("startedAtMs")), startedAt);
    }

    void differentCallIsBlockedActive()
    {
        JitsiCallStore store;
        QVariantMap first;
        first.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-first"));
        first.insert(QStringLiteral("locationName"), QStringLiteral("First"));
        first.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-a"));
        store.requestOpenCall(first);

        QVariantMap second = first;
        second.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-second"));
        second.insert(QStringLiteral("locationName"), QStringLiteral("Second"));
        const auto result = store.requestOpenCall(second);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("blocked-active"));
        QCOMPARE(store.activeCall().value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-first"));
    }

    void acceptInviteWhileActiveClearsInviteWithoutReplacingCall()
    {
        JitsiCallStore store;
        QVariantMap first;
        first.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-active"));
        first.insert(QStringLiteral("locationName"), QStringLiteral("Active"));
        first.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-a"));
        store.requestOpenCall(first);

        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 99);
        invite.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-invite"));
        invite.insert(QStringLiteral("callerName"), QStringLiteral("Invite"));
        invite.insert(QStringLiteral("locationName"), QStringLiteral("Invite"));
        invite.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-a"));
        store.ingestIncomingInvite(invite);

        store.acceptIncomingInvite();
        QCOMPARE(store.activeCall().value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-active"));
        QVERIFY(store.incomingInvite().isEmpty());
    }

    void acceptFreshInviteOpensCallWithDefaultMutedVideo()
    {
        JitsiCallStore store;
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 11);
        invite.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.workspace.example.com/workspace-room-11"));
        invite.insert(QStringLiteral("callerName"), QStringLiteral("Ku"));
        invite.insert(QStringLiteral("locationName"), QStringLiteral("Ku"));
        invite.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-a"));
        invite.insert(QStringLiteral("meetUrl"), QStringLiteral("https://meet.workspace.example.com"));
        invite.insert(QStringLiteral("displayName"), QStringLiteral("Current User"));
        store.ingestIncomingInvite(invite);
        store.acceptIncomingInvite();
        QVERIFY(store.incomingInvite().isEmpty());
        QCOMPARE(store.activeCall().value(QStringLiteral("locationName")).toString(), QStringLiteral("Ku"));
        QCOMPARE(store.activeCall().value(QStringLiteral("ownerKey")).toString(), QStringLiteral("owner-a"));
        QCOMPARE(store.activeCall().value(QStringLiteral("startWithVideoMuted")).toBool(), true);
        QCOMPARE(store.activeCall().value(QStringLiteral("callKey")).toString(),
            QStringLiteral("owner-a:https://meet.workspace.example.com/workspace-room-11"));
    }

    void defaultStartWithVideoMutedStaysTrue()
    {
        JitsiCallStore store;
        QVariantMap payload;
        payload.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.jit.si/room-m"));
        payload.insert(QStringLiteral("locationName"), QStringLiteral("Muted"));
        store.requestOpenCall(payload);
        QCOMPARE(store.activeCall().value(QStringLiteral("startWithVideoMuted")).toBool(), true);
    }

    void declineAndCloseAndClearResetState()
    {
        JitsiCallStore store;
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 12);
        invite.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.jit.si/room-d"));
        store.ingestIncomingInvite(invite);
        store.declineIncomingInvite();
        QVERIFY(store.incomingInvite().isEmpty());
        QVERIFY(!store.hasActiveCall());

        QVariantMap payload;
        payload.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.jit.si/room-e"));
        store.requestOpenCall(payload);
        QVERIFY(store.hasActiveCall());
        store.closeCall();
        QVERIFY(!store.hasActiveCall());
        QCOMPARE(store.requestOpenCall(payload).value(QStringLiteral("status")).toString(),
            QStringLiteral("opened"));
        store.clear();
        QVERIFY(!store.hasActiveCall());
    }
};

QTEST_MAIN(NativeCallTest)
#include "native-call-test.moc"
