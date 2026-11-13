#include "call/jitsi-call.h"
#include "call/native-call-store.h"
#include "call/native-call-window.h"
#include "call/incoming-call.h"
#include "call/incoming-call-lifecycle.h"

#include <QHash>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QXmlStreamReader>

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
    // --- Call window shell (issue #162, reference jitsi-call-modal-shell.hook.ts) ---

    QVariantMap validCall() const
    {
        QVariantMap call;
        call.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.jit.si/Room-X"));
        call.insert(QStringLiteral("locationName"), QStringLiteral("Design lobby"));
        return call;
    }

    void expandedBoundsMatchReferenceInsets()
    {
        // Golden acceptance: viewport 1440x900 -> x32 y32 1376x836.
        QCOMPARE(JitsiCallWindowController::expandedBounds(QSize(1440, 900)),
            QRect(32, 32, 1376, 836));
        // Both dims >= 640 use the 32 px desktop inset.
        QCOMPARE(JitsiCallWindowController::expandedBounds(QSize(640, 640)),
            QRect(32, 32, 576, 576));
        // One dim below 640 drops to the 16 px mobile inset.
        QCOMPARE(JitsiCallWindowController::expandedBounds(QSize(640, 500)),
            QRect(16, 16, 608, 468));
        QCOMPARE(JitsiCallWindowController::expandedBounds(QSize(639, 900)),
            QRect(16, 16, 607, 868));
        // Minimum window size 280x180 wins over the inset math.
        QCOMPARE(JitsiCallWindowController::expandedBounds(QSize(300, 200)),
            QRect(16, 16, 280, 180));
    }

    void pipDefaultBoundsAnchorBottomRight()
    {
        // Golden acceptance: PiP default x1100 y660 at 320x220 on 1440x900.
        QCOMPARE(JitsiCallWindowController::defaultPipBounds(QSize(1440, 900)),
            QRect(1100, 660, 320, 220));
        QCOMPARE(JitsiCallWindowController::defaultPipBounds(QSize(1280, 800)),
            QRect(940, 560, 320, 220));
        // Math.max(margin, ...) floors tiny viewports at the margin.
        QCOMPARE(JitsiCallWindowController::defaultPipBounds(QSize(300, 200)),
            QRect(20, 20, 320, 220));
    }

    void openShellParsesCallAndShowsLoading()
    {
        JitsiCallWindowController shell;
        shell.setViewportSize(QSize(1440, 900));
        shell.open(validCall());
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Expanded));
        QCOMPARE(shell.embedState(), int(JitsiCallWindowController::Loading));
        QCOMPARE(shell.bounds(), QRect(32, 32, 1376, 836));
        QCOMPARE(shell.meetingUrl(), QStringLiteral("https://meet.jit.si/Room-X"));
        QCOMPARE(shell.parsedRoomName(), QStringLiteral("Room-X"));
        QCOMPARE(shell.callName(), QStringLiteral("Design lobby"));
        QCOMPARE(shell.displayName(), QStringLiteral("Design lobby"));
        // One-time session config (jitsi-call.ui.tsx:15-21).
        const auto config = shell.sessionConfig();
        QCOMPARE(config.value(QStringLiteral("startWithAudioMuted")).toBool(), true);
        QCOMPARE(config.value(QStringLiteral("startWithVideoMuted")).toBool(), true);
        QCOMPARE(config.value(QStringLiteral("prejoinConfig")).toMap()
            .value(QStringLiteral("enabled")).toBool(), false);
        QCOMPARE(shell.subtitleText(), QString());
        QCOMPARE(shell.titleText(), QStringLiteral("Call - Design lobby"));
    }

    void sessionConfigHonoursVideoMutedPayloadAndDisplayNameFallback()
    {
        JitsiCallWindowController shell;
        QVariantMap call = validCall();
        call.insert(QStringLiteral("startWithVideoMuted"), false);
        call.remove(QStringLiteral("locationName"));
        call.insert(QStringLiteral("displayName"), QStringLiteral("  Ada  "));
        shell.open(call);
        QCOMPARE(shell.sessionConfig().value(QStringLiteral("startWithVideoMuted")).toBool(), false);
        QCOMPARE(shell.displayName(), QStringLiteral("Ada"));
        QCOMPARE(shell.callName(), QStringLiteral("Room-X"));

        JitsiCallWindowController bare;
        QVariantMap anon;
        anon.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.jit.si/Room-X"));
        bare.open(anon);
        QCOMPARE(bare.displayName(), QStringLiteral("Participant"));
    }

    void invalidUrlOpensShellInInvalidState()
    {
        JitsiCallWindowController shell;
        QVariantMap call;
        call.insert(QStringLiteral("meetingUrl"), QStringLiteral("definitely not a url"));
        shell.open(call);
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Expanded));
        QCOMPARE(shell.embedState(), int(JitsiCallWindowController::InvalidUrl));
        QCOMPARE(shell.stateText(), QStringLiteral("Invalid call link"));
        QCOMPARE(shell.embedCreations(), 0);
        // Without a call name the header falls back to the count form.
        QCOMPARE(shell.titleText(), QStringLiteral("Call"));
        shell.setParticipantCountProvider([] { return 3; });
        shell.pollNow();
        QCOMPARE(shell.titleText(), QStringLiteral("Call (3 participants)"));
    }

    void embedCreatedExactlyOnceAcrossShellToggles()
    {
        JitsiCallWindowController shell;
        shell.setViewportSize(QSize(1440, 900));
        int factoryCalls = 0;
        shell.setEmbedFactory([&factoryCalls] { ++factoryCalls; });
        shell.open(validCall());
        QCOMPARE(shell.embedCreations(), 0);
        shell.apiLoadSucceeded();
        QCOMPARE(shell.embedState(), int(JitsiCallWindowController::Ready));
        QCOMPARE(shell.embedCreations(), 1);
        // Minimize/expand/fullscreen/resize never recreate the embed
        // (reference mount-count contract in jitsi-call.ui.test.tsx).
        shell.toggleMinimize();
        shell.toggleMinimize();
        shell.toggleFullscreen();
        shell.setViewportSize(QSize(1000, 700));
        shell.setPipBounds(QRect(10, 20, 400, 300));
        QCOMPARE(shell.embedCreations(), 1);
        QCOMPARE(factoryCalls, 1);
        QCOMPARE(shell.embedState(), int(JitsiCallWindowController::Ready));
        shell.close();
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Closed));
    }

    void dismissMinimizesAndNeverClosesLiveCall()
    {
        JitsiCallWindowController shell;
        shell.setViewportSize(QSize(1440, 900));
        QSignalSpy closed(&shell, &JitsiCallWindowController::closeRequested);
        shell.open(validCall());
        shell.apiLoadSucceeded();
        shell.requestDismiss();
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Minimized));
        QCOMPARE(shell.bounds(), QRect(1100, 660, 320, 220));
        shell.requestDismiss();
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Minimized));
        QCOMPARE(closed.count(), 0);
        shell.toggleMinimize();
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Expanded));
        shell.requestDismiss();
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Minimized));
        QCOMPARE(closed.count(), 0);
        // Only readyToClose (or the close button) ends the call.
        shell.readyToClose();
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Closed));
        QCOMPARE(closed.count(), 1);
    }

    void fullscreenExitsWhenMinimizingAndIsIgnoredInPip()
    {
        JitsiCallWindowController shell;
        shell.open(validCall());
        shell.toggleFullscreen();
        QCOMPARE(shell.fullscreen(), true);
        shell.toggleMinimize();
        QCOMPARE(shell.fullscreen(), false);
        QCOMPARE(shell.mode(), int(JitsiCallWindowController::Minimized));
        shell.toggleFullscreen();
        QCOMPARE(shell.fullscreen(), false);
    }

    void pipBoundsPreservedAcrossTwoMinimizeCycles()
    {
        JitsiCallWindowController shell;
        shell.setViewportSize(QSize(1440, 900));
        shell.open(validCall());
        shell.toggleMinimize();
        QCOMPARE(shell.bounds(), QRect(1100, 660, 320, 220));
        shell.setPipBounds(QRect(100, 120, 400, 300));
        QCOMPARE(shell.bounds(), QRect(100, 120, 400, 300));
        for (int cycle = 0; cycle < 2; ++cycle) {
            shell.toggleMinimize();
            QCOMPARE(shell.bounds(), QRect(32, 32, 1376, 836));
            shell.toggleMinimize();
            QCOMPARE(shell.bounds(), QRect(100, 120, 400, 300));
        }
        // PiP stays draggable/resizable down to the 280x180 minimum.
        shell.setPipBounds(QRect(5, 5, 100, 100));
        QCOMPARE(shell.bounds(), QRect(5, 5, 280, 180));
        // A new call resets PiP to the default anchor.
        shell.close();
        shell.open(validCall());
        shell.toggleMinimize();
        QCOMPARE(shell.bounds(), QRect(1100, 660, 320, 220));
    }

    void participantEventsAndFocusAwarePollCadence()
    {
        JitsiCallWindowController shell;
        int participants = 1;
        shell.setParticipantCountProvider([&participants] { return participants; });
        shell.open(validCall());
        QCOMPARE(shell.participantCount(), -1);
        QCOMPARE(shell.pollIntervalMs(), 5000);
        shell.apiLoadSucceeded();
        QCOMPARE(shell.participantCount(), 1);
        participants = 4;
        shell.participantJoined();
        QCOMPARE(shell.participantCount(), 4);
        participants = 3;
        shell.participantLeft();
        QCOMPARE(shell.participantCount(), 3);
        participants = 7;
        shell.pollNow();
        QCOMPARE(shell.participantCount(), 7);
        shell.setFocused(false);
        QCOMPARE(shell.pollIntervalMs(), 30000);
        shell.setFocused(true);
        QCOMPARE(shell.pollIntervalMs(), 5000);
        shell.close();
        QCOMPARE(shell.participantCount(), -1);
    }

    void headerTitleAndSubtitleRules_data()
    {
        QTest::addColumn<QString>("locationName");
        QTest::addColumn<int>("count");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("subtitle");
        // Reference callName = trimmed locationName, else parsed room name;
        // so a valid URL always titles "Call - <name-or-room>" and the
        // count-form title belongs to the unparseable-URL header slot.
        QTest::newRow("named call, count unknown")
            << QStringLiteral("Design lobby") << -1
            << QStringLiteral("Call - Design lobby") << QString();
        QTest::newRow("named call with count") << QStringLiteral("Design lobby") << 3
            << QStringLiteral("Call - Design lobby") << QStringLiteral("3 participants");
        QTest::newRow("no location falls back to room name") << QString() << 1
            << QStringLiteral("Call - Room-X") << QStringLiteral("1 participant");
    }
    void headerTitleAndSubtitleRules()
    {
        QFETCH(QString, locationName);
        QFETCH(int, count);
        QFETCH(QString, title);
        QFETCH(QString, subtitle);
        JitsiCallWindowController shell;
        QVariantMap call;
        call.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.jit.si/Room-X"));
        call.insert(QStringLiteral("locationName"), locationName);
        if (count >= 0)
            shell.setParticipantCountProvider([count] { return count; });
        shell.open(call);
        if (count >= 0)
            shell.pollNow();
        QCOMPARE(shell.titleText(), title);
        QCOMPARE(shell.subtitleText(), subtitle);
    }

    void embedStatesAndHeaderLabelsUseReferenceTexts()
    {
        JitsiCallWindowController shell;
        shell.open(validCall());
        QCOMPARE(shell.stateText(), QStringLiteral("Loading call client\u2026"));
        shell.apiLoadFailed();
        QCOMPARE(shell.stateText(), QStringLiteral("Could not load call client"));
        QCOMPARE(shell.retryText(), QStringLiteral("Try again"));
        shell.retry();
        QCOMPARE(shell.embedState(), int(JitsiCallWindowController::Loading));
        QCOMPARE(shell.minimizeLabel(), QStringLiteral("Minimize"));
        QCOMPARE(shell.minimizeTooltip(), QStringLiteral("Minimize to window"));
        QCOMPARE(shell.fullscreenLabel(), QStringLiteral("Browser fullscreen mode"));
        shell.toggleFullscreen();
        QCOMPARE(shell.fullscreenLabel(), QStringLiteral("Exit fullscreen"));
        shell.toggleMinimize();
        QCOMPARE(shell.minimizeLabel(), QStringLiteral("Expand"));
        QCOMPARE(shell.minimizeTooltip(), QStringLiteral("Expand"));
        QCOMPARE(shell.closeLabel(), QStringLiteral("Close call"));
    }

    void permissionsGrantedOnceAndNeverRevokedByToggles()
    {
        JitsiCallWindowController shell;
        shell.open(validCall());
        QVERIFY(!shell.permissionsGranted());
        shell.apiLoadSucceeded();
        QVERIFY(shell.permissionsGranted());
        QCOMPARE(shell.allowPolicy(),
            QStringLiteral("camera; microphone; fullscreen; display-capture"));
        shell.toggleMinimize();
        shell.toggleFullscreen();
        shell.toggleMinimize();
        QVERIFY(shell.permissionsGranted());
    }

    // --- Review findings (#162 re-review) ---

    void isMeetingOriginAllowlistsTheMeetingOriginOnly()
    {
        // Finding 3: capture grants must follow the meeting origin, never the
        // requested feature alone. Deny-by-default allowlist.
        JitsiCallWindowController shell;
        shell.open(validCall()); // https://meet.jit.si/Room-X
        QVERIFY(shell.isMeetingOrigin(QStringLiteral("https://meet.jit.si")));
        QVERIFY(!shell.isMeetingOrigin(QStringLiteral("http://meet.jit.si")));
        QVERIFY(!shell.isMeetingOrigin(QStringLiteral("https://evil.example")));
        QVERIFY(!shell.isMeetingOrigin(QStringLiteral("https://meet.jit.si.evil.example")));
        QVERIFY(!shell.isMeetingOrigin(QStringLiteral("https://meet.jit.si:8443")));
        QVERIFY(!shell.isMeetingOrigin(QString()));
        // An invalid call never grants anything.
        JitsiCallWindowController invalid;
        QVariantMap bad;
        bad.insert(QStringLiteral("meetingUrl"), QStringLiteral("definitely not a url"));
        invalid.open(bad);
        QVERIFY(!invalid.isMeetingOrigin(QStringLiteral("https://evil.example")));
    }

    // Review finding (exo-review thread 4559430270): the port check must use
    // each URL's own scheme default, evaluated independently. A frame at
    // https://meet.example (effective 443) must NOT match a meeting at
    // https://meet.example:8443.
    void isMeetingOriginComparesEffectivePorts_data()
    {
        QTest::addColumn<QString>("meetingUrl");
        QTest::addColumn<QString>("origin");
        QTest::addColumn<bool>("expected");

        // Same host, meeting on a non-default port: a default-port frame is a
        // different origin and must get no grant.
        QTest::newRow("https-meeting-8443-vs-default-port-frame")
            << QStringLiteral("https://meet.jit.si:8443/Room-1")
            << QStringLiteral("https://meet.jit.si") << false;
        QTest::newRow("http-meeting-8443-vs-default-port-frame")
            << QStringLiteral("http://meet.jit.si:8443/Room-1")
            << QStringLiteral("http://meet.jit.si") << false;
        // Same host and port stays granted, spelled explicitly or not.
        QTest::newRow("https-meeting-8443-vs-same-explicit-port")
            << QStringLiteral("https://meet.jit.si:8443/Room-1")
            << QStringLiteral("https://meet.jit.si:8443") << true;
        // Default vs default: a scheme default is the same effective port.
        QTest::newRow("https-meeting-default-vs-default-port-frame")
            << QStringLiteral("https://meet.jit.si/Room-1")
            << QStringLiteral("https://meet.jit.si") << true;
        QTest::newRow("https-meeting-default-vs-explicit-443-frame")
            << QStringLiteral("https://meet.jit.si/Room-1")
            << QStringLiteral("https://meet.jit.si:443") << true;
        QTest::newRow("http-meeting-default-vs-default-port-frame")
            << QStringLiteral("http://meet.jit.si/Room-1")
            << QStringLiteral("http://meet.jit.si") << true;
        QTest::newRow("http-meeting-default-vs-explicit-80-frame")
            << QStringLiteral("http://meet.jit.si/Room-1")
            << QStringLiteral("http://meet.jit.si:80") << true;
        // A default-port meeting is not shared with a non-default-port frame.
        QTest::newRow("https-meeting-default-vs-8443-frame")
            << QStringLiteral("https://meet.jit.si/Room-1")
            << QStringLiteral("https://meet.jit.si:8443") << false;
        QTest::newRow("https-meeting-8443-vs-explicit-443-frame")
            << QStringLiteral("https://meet.jit.si:8443/Room-1")
            << QStringLiteral("https://meet.jit.si:443") << false;
        // Scheme default divergence: http:80 and https:443 never match.
        QTest::newRow("cross-scheme-default-port-divergence")
            << QStringLiteral("http://meet.jit.si/Room-1")
            << QStringLiteral("https://meet.jit.si") << false;
        // Scheme and host case-insensitivity still holds with ports.
        QTest::newRow("case-insensitive-scheme-host-with-port")
            << QStringLiteral("https://Meet.Jit.si:8443/Room-1")
            << QStringLiteral("HTTPS://meet.jit.si:8443") << true;
    }
    void isMeetingOriginComparesEffectivePorts()
    {
        QFETCH(QString, meetingUrl);
        QFETCH(QString, origin);
        QFETCH(bool, expected);
        JitsiCallWindowController shell;
        QVariantMap call;
        call.insert(QStringLiteral("meetingUrl"), meetingUrl);
        shell.open(call);
        QCOMPARE(shell.isMeetingOrigin(origin), expected);
    }

    void syncFullscreenTracksExternalWindowExit()
    {
        // Finding 4: the fullscreen flag must follow the real window, and
        // every transition must emit fullscreenChanged.
        JitsiCallWindowController shell;
        shell.open(validCall());
        shell.toggleFullscreen();
        QVERIFY(shell.fullscreen());
        QSignalSpy spy(&shell, &JitsiCallWindowController::fullscreenChanged);
        shell.syncFullscreen(true);   // window already fullscreen: no re-emit
        QCOMPARE(spy.count(), 0);
        shell.syncFullscreen(false);  // the window left fullscreen externally
        QCOMPARE(shell.fullscreen(), false);
        QCOMPARE(spy.count(), 1);
        // The fullscreen control is expanded-only; a minimized shell ignores it.
        shell.toggleMinimize();
        shell.syncFullscreen(true);
        QCOMPARE(shell.fullscreen(), false);
        QCOMPARE(spy.count(), 1);
    }

    void embedBridgeIsTheProductionLiveCountSource()
    {
        // Finding 5 (round 3): the reconcile timer must read a live
        // production source, not only a test-injected provider. The embed
        // bridge is that source: it models the reference external_api reads
        // and listeners (jitsi-participant-count.hook.ts:19-33), and the
        // production wiring attaches it as the controller's count provider.
        // Here the ONLY seam touched is the bridge - no setParticipantCountProvider,
        // no manual pollNow.
        JitsiCallWindowController shell;
        JitsiEmbedBridge bridge;
        bridge.attachTo(&shell);   // exactly what NativeController does in production
        shell.open(validCall());
        shell.apiLoadSucceeded();
        QCOMPARE(shell.participantCount(), -1);   // nothing reported by the embed yet
        bridge.reportParticipantCount(4);         // getNumberOfParticipants() from the embed
        QCOMPARE(shell.participantCount(), 4);
        QSignalSpy spy(&shell, &JitsiCallWindowController::participantCountChanged);
        bridge.reportParticipantCount(6);
        QCOMPARE(shell.participantCount(), 6);    // delivered without any manual poke
        QVERIFY(spy.count() >= 1);
        // The external_api listeners re-sync through the same bridge source.
        bridge.handleParticipantsJoined();
        bridge.handleParticipantsLeft();
        QCOMPARE(shell.participantCount(), 6);
        // The reconcile poll keeps working as reconciliation only: it re-reads
        // the bridge without a manual pollNow.
        shell.setPollIntervalMs(30);
        bridge.reportParticipantCount(9);
        QTRY_COMPARE_WITH_TIMEOUT(shell.participantCount(), 9, 2000);
        // Closing resets the live session exactly like the reference hook.
        shell.close();
        QCOMPARE(shell.participantCount(), -1);
        QCOMPARE(bridge.liveParticipantCount(), -1);
    }

    void embedBridgeCarriesTheSharedRosterSource()
    {
        // Reference: getParticipantsInfo() -> setParticipants(meetingUrl, ...)
        // shared store (jitsi-call-modal-shell.hook.ts:172-189). The native
        // counterpart is the bridge roster, fed from the same embed transport.
        JitsiCallWindowController shell;
        JitsiEmbedBridge bridge;
        bridge.attachTo(&shell);
        shell.open(validCall());
        shell.apiLoadSucceeded();
        bridge.reportRoster({QStringLiteral("Ada"), QStringLiteral("Grace")});
        QCOMPARE(bridge.liveRoster(), QStringList({QStringLiteral("Ada"), QStringLiteral("Grace")}));
        shell.close();
        QVERIFY(bridge.liveRoster().isEmpty());
    }

    void focusAwarePollReconcilesCountWithoutManualPoll()
    {
        // Finding 5: after Ready the controller owns a focus-aware reconcile
        // poll; a live count change reaches participantCount (and its NOTIFY)
        // without any manual pollNow call.
        JitsiCallWindowController shell;
        int participants = 2;
        shell.setParticipantCountProvider([&participants] { return participants; });
        shell.setPollIntervalMs(30);
        shell.open(validCall());
        shell.apiLoadSucceeded();
        QCOMPARE(shell.participantCount(), 2);
        QSignalSpy spy(&shell, &JitsiCallWindowController::participantCountChanged);
        participants = 5;
        QTRY_COMPARE_WITH_TIMEOUT(shell.participantCount(), 5, 2000);
        QVERIFY2(spy.count() >= 1, "participantCountChanged must fire on poll sync");
        // The poll keeps running while minimized (focus changes cadence only).
        shell.toggleMinimize();
        participants = 1;
        QTRY_COMPARE_WITH_TIMEOUT(shell.participantCount(), 1, 2000);
        // Closing stops the poll and resets the count.
        shell.close();
        participants = 9;
        QTest::qWait(200);
        QCOMPARE(shell.participantCount(), -1);
    }

    void defaultPipBoundsMapsThroughScreenOrigin()
    {
        // Finding 6: the default PiP rect is viewport-relative and must be
        // convertible to screen coords with the hosting window position.
        QCOMPARE(JitsiCallWindowController::defaultPipBounds(QSize(1100, 700), QPoint(120, 70)),
            QRect(880, 530, 320, 220));
        QCOMPARE(JitsiCallWindowController::defaultPipBounds(QSize(1100, 700), QPoint()),
            QRect(760, 460, 320, 220));
    }

    void russianCatalogTextsMatchReferenceLocale()
    {
        const auto path = QFINDTESTDATA("../translations/workspace_ru.ts");
        QVERIFY(!path.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QHash<QString, QString> translations;
        QXmlStreamReader xml(&file);
        QString context;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == QLatin1String("name"))
                context = xml.readElementText();
            else if (xml.name() == QLatin1String("message")) {
                const bool numerus = xml.attributes().value(QStringLiteral("numerus"))
                    == QLatin1String("yes");
                QString source;
                QString translation;
                while (xml.readNextStartElement()) {
                    if (xml.name() == QLatin1String("source"))
                        source = xml.readElementText();
                    else if (xml.name() == QLatin1String("translation")) {
                        if (!numerus) {
                            translation = xml.readElementText();
                        } else {
                            QStringList numerusForms;
                            while (xml.readNextStartElement()) {
                                if (xml.name() == QLatin1String("numerusform"))
                                    numerusForms.append(xml.readElementText());
                                else
                                    xml.skipCurrentElement();
                            }
                            translation = numerusForms.join(QLatin1Char('\x1f'));
                        }
                    } else
                        xml.skipCurrentElement();
                }
                if (context == QLatin1String("JitsiCallWindowController"))
                    translations.insert(source, translation);
            }
        }
        // Reference packages/web/src/i18n/locales/ru.json at the contract sha,
        // with {{count}} mapped to the Qt numerus %n.
        const QHash<QString, QString> expected = {
            {QStringLiteral("Call"), QString::fromUtf8("Позвонить")},
            {QStringLiteral("Participant"), QString::fromUtf8("Участник")},
            {QStringLiteral("Invalid call link"), QString::fromUtf8("Неверная ссылка на звонок")},
            {QStringLiteral("Loading call client\u2026"),
                QString::fromUtf8("Загрузка клиента звонков…")},
            {QStringLiteral("Could not load call client"),
                QString::fromUtf8("Не удалось загрузить клиент звонков")},
            {QStringLiteral("Try again"), QString::fromUtf8("Попробовать снова")},
            {QStringLiteral("Browser fullscreen mode"),
                QString::fromUtf8("Полноэкранный режим браузера")},
            {QStringLiteral("Exit fullscreen"), QString::fromUtf8("Выйти из полноэкранного")},
            {QStringLiteral("Expand"), QString::fromUtf8("Развернуть")},
            {QStringLiteral("Minimize"), QString::fromUtf8("Свернуть")},
            {QStringLiteral("Minimize to window"), QString::fromUtf8("Свернуть в окно")},
            {QStringLiteral("Close call"), QString::fromUtf8("Закрыть звонок")},
            {QStringLiteral("Call (%n participant)"),
                QString::fromUtf8("Звонок (%n участник)\x1fЗвонок (%n участника)"
                                  "\x1fЗвонок (%n участников)")},
            {QStringLiteral("Call (%n participants)"),
                QString::fromUtf8("Звонок (%n участник)\x1fЗвонок (%n участника)"
                                  "\x1fЗвонок (%n участников)")},
            {QStringLiteral("%n participant"),
                QString::fromUtf8("%n участник\x1f%n участника\x1f%n участников")},
            {QStringLiteral("%n participants"),
                QString::fromUtf8("%n участник\x1f%n участника\x1f%n участников")},
        };
        for (const auto &key : expected.keys())
            QVERIFY2(translations.value(key) == expected.value(key),
                qPrintable(key + QStringLiteral(" != expected ru text")));
    }

    // --- Incoming call invite: accept option overload (issue #163) ---

    void acceptIncomingInviteOptionControlsStartWithVideoMuted()
    {
        // jitsi-call.model.ts:163-193 + jitsi-call-shell.ui.tsx:61-63:
        // accept passes startWithVideoMuted = !videoEnabled (default muted).
        JitsiCallStore store;
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 21);
        invite.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-21"));
        invite.insert(QStringLiteral("locationName"), QStringLiteral("Direct"));
        store.ingestIncomingInvite(invite);

        QVariantMap options;
        options.insert(QStringLiteral("startWithVideoMuted"), false);
        const auto opened = store.acceptIncomingInvite(options);
        QCOMPARE(opened.value(QStringLiteral("startWithVideoMuted")).toBool(), false);
        QCOMPARE(store.activeCall().value(QStringLiteral("startWithVideoMuted")).toBool(), false);
        QVERIFY(store.incomingInvite().isEmpty());
    }

    void acceptIncomingInviteEmptyOptionKeepsVideoMuted()
    {
        // jitsi-call.model.ts:166 (startWithVideoMuted ?? true).
        JitsiCallStore store;
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 22);
        invite.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-22"));
        store.ingestIncomingInvite(invite);
        const auto opened = store.acceptIncomingInvite(QVariantMap{});
        QCOMPARE(opened.value(QStringLiteral("startWithVideoMuted")).toBool(), true);
    }

    // --- Ingest matrix from jitsi-call.model.test.ts:172-231 ---

    void ingestIncomingInviteDeduplicatesByMessageId()
    {
        // jitsi-call.model.ts:138-141: the same message id never re-raises.
        JitsiCallStore store;
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 31);
        invite.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-31"));
        store.ingestIncomingInvite(invite);
        QCOMPARE(store.incomingInvite().value(QStringLiteral("messageId")).toInt(), 31);

        QVariantMap same;
        same.insert(QStringLiteral("messageId"), 31);
        same.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-31-later"));
        store.ingestIncomingInvite(same);
        QCOMPARE(store.incomingInvite().value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.jit.si/room-31"));
    }

    void ingestIncomingInviteIgnoredWhileCallActive()
    {
        // jitsi-call.model.ts:142-148: the id is remembered but nothing shows.
        JitsiCallStore store;
        QVariantMap payload;
        payload.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-live"));
        store.requestOpenCall(payload);
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"), 32);
        invite.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-32"));
        store.ingestIncomingInvite(invite);
        QVERIFY(store.incomingInvite().isEmpty());
        // The dedupe id was recorded like the reference ("lastIncomingMessageId"
        // is set even on the ignored path).
        store.closeCall();
        store.ingestIncomingInvite(invite);
        QVERIFY(store.incomingInvite().isEmpty());
    }

    void firstInviteKeptWhileAnotherArrives()
    {
        // jitsi-call.model.ts:147-153: a second invite never replaces the shown one.
        JitsiCallStore store;
        QVariantMap first;
        first.insert(QStringLiteral("messageId"), 33);
        first.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-33"));
        QVariantMap second;
        second.insert(QStringLiteral("messageId"), 34);
        second.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.jit.si/room-34"));
        store.ingestIncomingInvite(first);
        store.ingestIncomingInvite(second);
        QCOMPARE(store.incomingInvite().value(QStringLiteral("messageId")).toInt(), 33);
    }

    // --- Invite builder (workspace-jitsi-incoming-call.lib.ts:40-73) ---

    void inviteBuilderRejectsNonInviteMessages_data()
    {
        QTest::addColumn<QString>("caseName");
        QTest::addColumn<QVariantMap>("override");
        QTest::newRow("read") << QStringLiteral("read")
            << (QVariantMap{{QStringLiteral("messageRead"), true}});
        QTest::newRow("own") << QStringLiteral("own")
            << (QVariantMap{{QStringLiteral("messageOwn"), true}});
        QTest::newRow("self-author") << QStringLiteral("self-author")
            << (QVariantMap{{QStringLiteral("authorUuid"),
                    QStringLiteral("11111111-1111-1111-1111-111111111111")}});
        QTest::newRow("no-stream") << QStringLiteral("no-stream")
            << (QVariantMap{{QStringLiteral("streamKnown"), false}});
        QTest::newRow("not-private") << QStringLiteral("not-private")
            << (QVariantMap{{QStringLiteral("streamPrivate"), false}});
        QTest::newRow("no-direct-user") << QStringLiteral("no-direct-user")
            << (QVariantMap{{QStringLiteral("streamDirectUser"), QString()}});
        QTest::newRow("encrypted-payload") << QStringLiteral("encrypted-payload")
            << (QVariantMap{{QStringLiteral("payloadKind"), QStringLiteral("encrypted")}});
        QTest::newRow("foreign-host") << QStringLiteral("foreign-host")
            << (QVariantMap{{QStringLiteral("content"),
                    QStringLiteral("https://evil.example/room-9")}});
    }
    void inviteBuilderRejectsNonInviteMessages()
    {
        QFETCH(QString, caseName);
        QFETCH(QVariantMap, override);
        Q_UNUSED(caseName);
        QVariantMap input = inviteBuilderGoldenInput();
        for (const auto &key : override.keys())
            input.insert(key, override.value(key));
        QVERIFY2(buildIncomingDmCallInvite(input).isEmpty(),
            qPrintable(QStringLiteral("case %1 must not raise an invite").arg(caseName)));
    }

    void inviteBuilderMapsFieldsAndCallerFallback()
    {
        // lib.ts:23-34, 58-72: display name -> stream name -> author prefix.
        QVariantMap input = inviteBuilderGoldenInput();
        const auto invite = buildIncomingDmCallInvite(input);
        QCOMPARE(invite.value(QStringLiteral("messageId")).toString(),
            QStringLiteral("aaaa0000-0000-0000-0000-000000000009"));
        QCOMPARE(invite.value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.jit.si/room-9"));
        QCOMPARE(invite.value(QStringLiteral("callerName")).toString(),
            QStringLiteral("Slon"));
        QCOMPARE(invite.value(QStringLiteral("locationName")).toString(),
            QStringLiteral("Slon Direct"));
        QCOMPARE(invite.value(QStringLiteral("avatarUrl")).toString(),
            QStringLiteral("urn:avatar:slon"));

        QVariantMap noDisplay = inviteBuilderGoldenInput();
        noDisplay.insert(QStringLiteral("authorDisplayName"), QString());
        QCOMPARE(buildIncomingDmCallInvite(noDisplay)
            .value(QStringLiteral("callerName")).toString(), QStringLiteral("Slon Direct"));

        QVariantMap noStream = inviteBuilderGoldenInput();
        noStream.insert(QStringLiteral("authorDisplayName"), QString());
        noStream.insert(QStringLiteral("streamName"), QString());
        QCOMPARE(buildIncomingDmCallInvite(noStream)
            .value(QStringLiteral("callerName")).toString(), QStringLiteral("aaaa0000"));
    }

    // --- Variant flag (shared/lib/env.ts:132-135) ---

    void resolveIncomingCallVariantMatchesReferenceEnv()
    {
        QCOMPARE(resolveIncomingCallVariant(QString{}), QStringLiteral("large"));
        QCOMPARE(resolveIncomingCallVariant(QStringLiteral("large")), QStringLiteral("large"));
        QCOMPARE(resolveIncomingCallVariant(QStringLiteral("compact")), QStringLiteral("compact"));
        QCOMPARE(resolveIncomingCallVariant(QStringLiteral("COMPACT")), QStringLiteral("compact"));
        QCOMPARE(resolveIncomingCallVariant(QStringLiteral("Compact")), QStringLiteral("compact"));
        // Invalid values fall back to large (env.ts:134).
        QCOMPARE(resolveIncomingCallVariant(QStringLiteral("toast")), QStringLiteral("large"));
        QCOMPARE(resolveIncomingCallVariant(QStringLiteral("")), QStringLiteral("large"));
    }

    // --- Lifecycle (jitsi-call-incoming-lifecycle.hook.ts:6-8, 21, 28-58) ---

    void lifecycleDefaultsUseReferenceConstants()
    {
        IncomingCallLifecycle lifecycle;
        // INCOMING_INVITE_RING_INTERVAL_MS = 1_500 / AUTO_DECLINE = 45_000.
        QCOMPARE(lifecycle.ringIntervalMs(), 1500);
        QCOMPARE(lifecycle.autoDeclineMs(), 45000);
        QCOMPARE(lifecycle.notificationSound(), QStringLiteral("default"));
        QCOMPARE(lifecycle.visible(), false);
    }

    void lifecycleRingsImmediatelyThenRepeatedlyWhileVisible()
    {
        IncomingCallLifecycle lifecycle;
        JitsiCallStore store;
        lifecycle.attachStore(&store);
        lifecycle.setRingIntervalMs(20);
        QSignalSpy rings(&lifecycle, &IncomingCallLifecycle::ringRequested);
        QVERIFY(rings.isValid());
        store.ingestIncomingInvite(lifecycleInvite());
        QVERIFY(lifecycle.visible());
        QVERIFY2(rings.count() >= 1, "the ring fires immediately on becoming visible");
        QTRY_VERIFY_WITH_TIMEOUT(rings.count() >= 3, 2000);
        store.declineIncomingInvite();
        const auto frozen = rings.count();
        QTest::qWait(100);
        QCOMPARE(rings.count(), frozen);
    }

    void lifecycleRingSuppressedWhenNotificationSoundNone()
    {
        // lifecycle.hook.ts:43-46: the "none" preset never plays.
        IncomingCallLifecycle lifecycle;
        JitsiCallStore store;
        lifecycle.attachStore(&store);
        lifecycle.setRingIntervalMs(20);
        lifecycle.setNotificationSound(QStringLiteral("none"));
        QSignalSpy rings(&lifecycle, &IncomingCallLifecycle::ringRequested);
        QVERIFY(rings.isValid());
        store.ingestIncomingInvite(lifecycleInvite());
        QVERIFY(lifecycle.visible());
        QTest::qWait(120);
        QCOMPARE(rings.count(), 0);
    }

    void lifecycleAutoDeclineClearsInviteAndNeverOpensCall()
    {
        // lifecycle.hook.ts:28-36: single-shot decline at the timeout.
        IncomingCallLifecycle lifecycle;
        JitsiCallStore store;
        lifecycle.attachStore(&store);
        lifecycle.setAutoDeclineMs(40);
        lifecycle.setRingIntervalMs(5000);
        QSignalSpy declined(&lifecycle, &IncomingCallLifecycle::autoDeclined);
        QVERIFY(declined.isValid());
        store.ingestIncomingInvite(lifecycleInvite());
        QVERIFY(lifecycle.visible());
        QTRY_VERIFY_WITH_TIMEOUT(declined.count() == 1, 2000);
        QVERIFY(store.incomingInvite().isEmpty());
        QVERIFY(!store.hasActiveCall());
        QVERIFY(!lifecycle.visible());
    }

    void lifecycleHidingCancelsRingAndAutoDecline()
    {
        IncomingCallLifecycle lifecycle;
        JitsiCallStore store;
        lifecycle.attachStore(&store);
        lifecycle.setAutoDeclineMs(60);
        lifecycle.setRingIntervalMs(20);
        QSignalSpy rings(&lifecycle, &IncomingCallLifecycle::ringRequested);
        QSignalSpy declined(&lifecycle, &IncomingCallLifecycle::autoDeclined);
        QVERIFY(rings.isValid() && declined.isValid());
        store.ingestIncomingInvite(lifecycleInvite());
        QTRY_VERIFY_WITH_TIMEOUT(rings.count() >= 1, 1000);
        lifecycle.accept();
        QVERIFY(!lifecycle.visible());
        const auto frozen = rings.count();
        QTest::qWait(200);
        QCOMPARE(rings.count(), frozen);
        QCOMPARE(declined.count(), 0);
        QVERIFY(store.hasActiveCall());
    }

    void lifecycleVideoPreferencePerMessageIdAndAccept()
    {
        // jitsi-call-shell.ui.tsx:41-44, 61-74: preference is per message id,
        // accept maps it to startWithVideoMuted and clears it.
        IncomingCallLifecycle lifecycle;
        JitsiCallStore store;
        lifecycle.attachStore(&store);
        lifecycle.setAutoDeclineMs(60000);
        const auto messageId = QStringLiteral("aaaa0000-0000-0000-0000-000000000009");
        QVERIFY(!lifecycle.videoEnabled(messageId));
        store.ingestIncomingInvite(lifecycleInvite());
        lifecycle.toggleVideoEnabled(messageId);
        QVERIFY(lifecycle.videoEnabled(messageId));
        // A toggle for another invite id does not leak into this one.
        QVERIFY(!lifecycle.videoEnabled(QStringLiteral("other-id")));
        lifecycle.accept();
        QCOMPARE(store.activeCall().value(QStringLiteral("startWithVideoMuted")).toBool(), false);
        QVERIFY(!lifecycle.videoEnabled(messageId));
    }

    void lifecycleDeclineTimerUsesPreciseType_data()
    {
        QTest::addColumn<bool>("armed");
        QTest::newRow("idle") << false;
        QTest::newRow("armed") << true;
    }

    void lifecycleDeclineTimerUsesPreciseType()
    {
        // PR #257 finding 5: the default coarse timer may fire the 45 s
        // auto-decline up to 5% early; the timer type must be Precise and
        // its interval must match the observable autoDeclineMs (45000 by
        // default) so the deadline cannot drift.
        QFETCH(bool, armed);
        IncomingCallLifecycle lifecycle;
        JitsiCallStore store;
        lifecycle.attachStore(&store);
        lifecycle.setRingIntervalMs(600000);
        if (armed)
            store.ingestIncomingInvite(lifecycleInvite());
        QVERIFY(lifecycle.visible() == armed);
        QCOMPARE(lifecycle.declineTimerType(), Qt::PreciseTimer);
        QCOMPARE(lifecycle.declineTimerInterval(), lifecycle.autoDeclineMs());
    }

    void lifecycleInviteSwapResetsVideoPreference()
    {
        // PR #257 finding 6: when the current invite changes (cleared and
        // replaced by a later one), the discarded invite's video preference
        // must not survive into a replay, and the video binding's NOTIFY
        // (videoPreferenceChanged) must fire on the invite swap.
        IncomingCallLifecycle lifecycle;
        JitsiCallStore store;
        lifecycle.attachStore(&store);
        lifecycle.setRingIntervalMs(600000);
        lifecycle.setAutoDeclineMs(600000);
        const auto firstId = QStringLiteral("aaaa0000-0000-0000-0000-000000000009");
        store.ingestIncomingInvite(lifecycleInvite());
        QSignalSpy preference(&lifecycle, &IncomingCallLifecycle::videoPreferenceChanged);
        QVERIFY(preference.isValid());
        lifecycle.toggleVideoEnabled(firstId);
        QVERIFY(lifecycle.videoEnabled(firstId));
        QVERIFY(lifecycle.currentVideoEnabled());
        const auto before = preference.count();
        // The invite disappears without an accept/decline/dismiss handler
        // (expired/revoked path clears the store directly), then a new invite
        // arrives without any video toggle.
        store.clearIncomingInvite();
        QVariantMap second = lifecycleInvite();
        second.insert(QStringLiteral("messageId"),
            QStringLiteral("bbbb0000-0000-0000-0000-00000000000b"));
        store.ingestIncomingInvite(second);
        QVERIFY(lifecycle.visible());
        QVERIFY2(preference.count() > before,
            "the video binding must be notified when the current invite changes");
        QVERIFY(!lifecycle.currentVideoEnabled());
        // A replay of the discarded invite starts from the default (off).
        QVERIFY(!lifecycle.videoEnabled(firstId));
        store.declineIncomingInvite();
        store.ingestIncomingInvite(lifecycleInvite());
        QVERIFY(lifecycle.visible());
        QVERIFY(!lifecycle.videoEnabled(firstId));
        QVERIFY(!lifecycle.currentVideoEnabled());
    }

    // #250/#251/#252: source-contract guards for the shipped embed sources.
    // The behavioral proof lives in the window-class cases of
    // native-window-test.cpp (real WebEngine surface); these pin the
    // contracts themselves so the invalid Qt/Jitsi usages cannot silently
    // return: the Qt 6.10.2 reality is QWebEngineLoadingInfo LoadStatus
    // constants on loadingChanged (qwebengineloadinginfo.h:35-40), the
    // QWebEnginePermission object grant()/deny() on permissionRequested
    // (qwebenginepermission.h), and the Jitsi EXTERNAL API wire contract -
    // never the internal API object, never methods the runtime lacks.

    void shippedEmbedViewUsesRealQt610LoadStatusConstants()
    {
        const auto src = readEmbedSource(QStringLiteral("NativeCallWebEngineView.qml"));
        QVERIFY2(!src.isEmpty(), "the shipped embed view source must be locatable");
        QVERIFY2(!src.contains(QStringLiteral("LoadFinishedStatus")),
            "WebEngineView.LoadFinishedStatus does not exist in Qt 6.10.2 (#250)");
        QVERIFY2(!src.contains(QStringLiteral("loadRequest.error.code")),
            "QWebEngineLoadingInfo exposes errorCode/errorString, not error.code (#250)");
        QVERIFY(src.contains(QStringLiteral("loadStatusSucceeded")));
        QVERIFY(src.contains(QStringLiteral("loadStatusFailed")));
    }

    void shippedEmbedViewUsesTheQt610PermissionObject()
    {
        const auto src = readEmbedSource(QStringLiteral("NativeCallWebEngineView.qml"));
        QVERIFY2(!src.isEmpty(), "the shipped embed view source must be locatable");
        QVERIFY2(!src.contains(QStringLiteral("denyPermission"))
                 && !src.contains(QStringLiteral("grantPermission")),
            "WebEngineView has no grantPermission/denyPermission in Qt 6.10.2 (#251)");
        QVERIFY2(!src.contains(QStringLiteral("WebEngineProfile.AudioCapture")),
            "WebEngineProfile capture feature enums do not exist (#251)");
        QVERIFY(src.contains(QStringLiteral("onPermissionRequested")));
        QVERIFY(src.contains(QStringLiteral("permission.grant()")));
        QVERIFY(src.contains(QStringLiteral("permission.deny()")));
    }

    void shippedCallEventGlueSpeaksOnlyTheExternalApiContract()
    {
        const auto src = readEmbedSource(QStringLiteral("web/workspace-call-events.js"));
        QVERIFY2(!src.isEmpty(), "the shipped call-event glue must be locatable");
        QVERIFY2(!src.contains(QStringLiteral("APP.API")),
            "the glue must never touch the Jitsi-meet internal API object (#252)");
        QVERIFY2(src.contains(QStringLiteral("postis")),
            "the glue must speak the pinned external-API postMessage wire contract (#252)");
    }

    void shippedCallEventGlueBindsTransportEventsToTrustedSourceAndOrigin()
    {
        // Issue #261 review: shape+scope validation let any child frame post
        // a matching event and corrupt the roster/count or fire join/leave
        // handlers. The listener must bind every transport event to the
        // expected meeting window (event.source) AND the loaded meeting
        // origin (event.origin), and drop everything else.
        const auto src = readEmbedSource(QStringLiteral("web/workspace-call-events.js"));
        QVERIFY2(!src.isEmpty(), "the shipped call-event glue must be locatable");
        QVERIFY2(src.contains(QStringLiteral("event.source")),
            "transport events must be bound to the expected meeting window (#261)");
        QVERIFY2(src.contains(QStringLiteral("event.origin")),
            "transport events must be bound to the meeting origin (#261)");
    }

    void shippedCallEventGlueAnswersEachForeignReadyIdAtMostOnce()
    {
        // Issue #261 review: the ready handler echoed every unfamiliar
        // readiness ID with postMessage to the SAME window, received its own
        // echo, and echoed again - an endless self-posting loop. The listener
        // must only answer readiness IDs it has not answered yet.
        const auto src = readEmbedSource(QStringLiteral("web/workspace-call-events.js"));
        QVERIFY2(!src.isEmpty(), "the shipped call-event glue must be locatable");
        QVERIFY2(!src.contains(QStringLiteral("else {\n                wire(\"__ready__\", data.params);\n            }")),
            "an unfamiliar ready ID must never be echoed unconditionally (#261 echo loop)");
    }

private:
    static QString readEmbedSource(const QString &name)
    {
#ifdef WORKSPACE_NATIVE_CALL_QML_SOURCE_DIR
        QFile file(QStringLiteral(WORKSPACE_NATIVE_CALL_QML_SOURCE_DIR "/") + name);
        if (file.open(QIODevice::ReadOnly))
            return QString::fromUtf8(file.readAll());
#else
        Q_UNUSED(name);
#endif
        return QString();
    }

    static QVariantMap inviteBuilderGoldenInput()
    {
        QVariantMap input;
        input.insert(QStringLiteral("messageId"),
            QStringLiteral("aaaa0000-0000-0000-0000-000000000009"));
        input.insert(QStringLiteral("messageRead"), false);
        input.insert(QStringLiteral("messageOwn"), false);
        input.insert(QStringLiteral("authorUuid"),
            QStringLiteral("aaaa0000-0000-0000-0000-000000000009"));
        input.insert(QStringLiteral("payloadKind"), QStringLiteral("markdown"));
        input.insert(QStringLiteral("content"), QStringLiteral("https://meet.jit.si/room-9"));
        input.insert(QStringLiteral("streamKnown"), true);
        input.insert(QStringLiteral("streamName"), QStringLiteral("Slon Direct"));
        input.insert(QStringLiteral("streamPrivate"), true);
        input.insert(QStringLiteral("streamDirectUser"),
            QStringLiteral("aaaa0000-0000-0000-0000-000000000009"));
        input.insert(QStringLiteral("authorDisplayName"), QStringLiteral("Slon"));
        input.insert(QStringLiteral("authorAvatarUrl"), QStringLiteral("urn:avatar:slon"));
        input.insert(QStringLiteral("currentUserUuid"),
            QStringLiteral("11111111-1111-1111-1111-111111111111"));
        input.insert(QStringLiteral("currentUserDisplayName"), QStringLiteral("Me"));
        input.insert(QStringLiteral("ownerKey"), QStringLiteral("owner-1"));
        input.insert(QStringLiteral("meetUrl"), QStringLiteral("https://meet.jit.si"));
        return input;
    }

    static QVariantMap lifecycleInvite()
    {
        QVariantMap invite;
        invite.insert(QStringLiteral("messageId"),
            QStringLiteral("aaaa0000-0000-0000-0000-000000000009"));
        invite.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.jit.si/room-9"));
        invite.insert(QStringLiteral("callerName"), QStringLiteral("Slon"));
        invite.insert(QStringLiteral("locationName"), QStringLiteral("Slon Direct"));
        return invite;
    }
};

QTEST_MAIN(NativeCallTest)
#include "native-call-test.moc"
