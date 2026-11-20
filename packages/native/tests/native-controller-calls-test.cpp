#include "call/native-call-window.h"
#include "call/display-capture-policy.h"
#include "call/screen-share-source-picker.h"
#include "support/native-test-harness.h"
#include "support/controller-chat-creation-fixtures.h"

#include <QDBusMetaType>

class PortalRequestStub;

// #87: minimal org.freedesktop.portal.Desktop / Screenshot stand-in on the
// session bus, matching the contract the picker's default transport drives:
// PictureUri(parent_window s, options a{sv}) -> o request path, then that
// Request object emits Response(u code, a{sv} results) whose PictureUri
// member arrives wrapped in a variant (the QDBusVariant form).
class PortalScreenshotStub final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    QString pictureUri = QStringLiteral("file:///tmp/lane87-portal-shot.png");
    quint32 responseCode = 0; // 0 succeeded, 1 cancelled (portal semantics)
    QString requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/87/87");
    int pictureUriCalls = 0;
    int responseDelayMs = 250;
    PortalRequestStub *requestObject = nullptr;
};

class PortalRequestStub final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    QString path; // path the test registered this object at (#87 round 2)
    int closeCalls = 0;
};

class PortalRequestAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    explicit PortalRequestAdaptor(PortalRequestStub *request) : QDBusAbstractAdaptor(request) {}
public slots:
    void Close() { ++static_cast<PortalRequestStub *>(parent())->closeCalls; }
signals:
    void Response(quint32 code, const QVariantMap &results);
};

class PortalScreenshotAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
public:
    PortalScreenshotAdaptor(PortalScreenshotStub *stub, QObject *registered)
        : QDBusAbstractAdaptor(registered), m_stub(stub) {}

public slots:
    QDBusObjectPath PictureUri(const QString &parentWindow, const QVariantMap &options);

private:
    PortalScreenshotStub *m_stub;
};

QDBusObjectPath PortalScreenshotAdaptor::PictureUri(const QString &parentWindow, const QVariantMap &options)
{
    Q_UNUSED(parentWindow)
    Q_UNUSED(options)
    ++m_stub->pictureUriCalls;
    auto *request = m_stub->requestObject;
    auto *adaptor = request->findChild<PortalRequestAdaptor *>();
    // The portal answers after the request object path is already in the
    // client's hands; a short delay keeps the emission ordered after the
    // client returns from the blocking call and subscribes.
    const int delayMs = m_stub->responseDelayMs;
    QTimer::singleShot(delayMs, request, [request, adaptor, stub = m_stub] {
        const QVariantMap results {
            {QStringLiteral("PictureUri"), QVariant::fromValue(QDBusVariant(stub->pictureUri))}
        };
        adaptor->Response(stub->responseCode, results);
    });
    return QDBusObjectPath(m_stub->requestPath);
}

// Stand-in for the public org.freedesktop.portal.ScreenCast interface.
// Start returns the documented a(ua{sv}) stream tuples over the real bus.
using FixturePortalStream = std::pair<quint32, QVariantMap>;

class PortalSessionStub final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
public:
    int closeCalls = 0;
public slots:
    void Close() { ++closeCalls; }
};

class PortalScreenCastStub final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    std::function<void(const QString &)> sessionCreated;
    int createSessionCalls = 0;
    int selectSourcesCalls = 0;
    int startCalls = 0;
    QString selectedSession;
    QString startedSession;
    quint32 types = 0;
    bool validTokens = true;
    int responseDelayMs = 120;
    quint32 startResponseCode = 0; // 1 is the user cancelling in the chooser
    QString sessionPath = QStringLiteral("/org/freedesktop/portal/desktop/session/87/session");
    const quint32 nodeId = 73;
    const quint64 streamSerial = 18023;
    // Each step gets its own Request object from the pool the test registered.
    QList<PortalRequestStub *> requestPool;
    int requestCursor = 0;
    PortalRequestStub *takeRequest()
    {
        PortalRequestStub *request = requestPool.at(requestCursor % requestPool.size());
        ++requestCursor;
        return request;
    }
};

class PortalScreenCastAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
public:
    PortalScreenCastAdaptor(PortalScreenCastStub *stub, QObject *registered)
        : QDBusAbstractAdaptor(registered), m_stub(stub) {}

public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options);
    QDBusObjectPath SelectSources(const QDBusObjectPath &session, const QVariantMap &options);
    QDBusObjectPath Start(const QDBusObjectPath &session, const QString &parentWindow, const QVariantMap &options);

private:
    QDBusObjectPath answerWith(const QVariantMap &results, quint32 code = 0);
    PortalScreenCastStub *m_stub;
};

QDBusObjectPath PortalScreenCastAdaptor::answerWith(const QVariantMap &results, quint32 code)
{
    auto *request = m_stub->takeRequest();
    auto *adaptor = request->findChild<PortalRequestAdaptor *>();
    const int delayMs = m_stub->responseDelayMs;
    if (delayMs < 0)
        adaptor->Response(code, results);
    else
        QTimer::singleShot(delayMs, request, [adaptor, code, results] { adaptor->Response(code, results); });
    return QDBusObjectPath(request->path);
}

QDBusObjectPath PortalScreenCastAdaptor::CreateSession(const QVariantMap &options)
{
    ++m_stub->createSessionCalls;
    const QRegularExpression validToken(QStringLiteral("^[a-zA-Z0-9_]+$"));
    m_stub->validTokens = validToken.match(options.value("handle_token").toString()).hasMatch()
        && validToken.match(options.value("session_handle_token").toString()).hasMatch();
    if (m_stub->sessionCreated)
        m_stub->sessionCreated(options.value("session_handle_token").toString());
    return answerWith({{"session_handle", m_stub->sessionPath}});
}

QDBusObjectPath PortalScreenCastAdaptor::SelectSources(const QDBusObjectPath &session,
    const QVariantMap &options)
{
    ++m_stub->selectSourcesCalls;
    m_stub->selectedSession = session.path();
    m_stub->types = options.value("types").toUInt();
    return answerWith({});
}

QDBusObjectPath PortalScreenCastAdaptor::Start(const QDBusObjectPath &session,
    const QString &parentWindow, const QVariantMap &options)
{
    Q_UNUSED(parentWindow)
    Q_UNUSED(options)
    ++m_stub->startCalls;
    m_stub->startedSession = session.path();
    qDBusRegisterMetaType<FixturePortalStream>();
    qDBusRegisterMetaType<QList<FixturePortalStream>>();
    const QList<FixturePortalStream> streams{{m_stub->nodeId,
        {{"source_type", 1u}, {"pipewire-serial", QVariant::fromValue(m_stub->streamSerial)}}}};
    return answerWith({{"streams", QVariant::fromValue(streams)}}, m_stub->startResponseCode);
}

// Registers the per-step Request pool the ScreenCast stub hands out.
class PortalRequestPool {
public:
    bool registerOn(QDBusConnection &bus, const QString &base, int count,
        PortalScreenCastStub &stub)
    {
        for (int i = 0; i < count; ++i) {
            auto *request = new PortalRequestStub;
            request->path = base + QString::number(i);
            new PortalRequestAdaptor(request);
            if (!bus.registerObject(request->path, request)) {
                unregisterFrom(bus);
                return false;
            }
            requests.append(request);
            stub.requestPool.append(request);
        }
        return true;
    }
    void unregisterFrom(QDBusConnection &bus)
    {
        for (auto *request : requests)
            bus.unregisterObject(request->path);
        qDeleteAll(requests);
        requests.clear();
    }
    QList<PortalRequestStub *> requests;
};

class ControllerCallsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Issue #161 call launch (docs/NATIVE_DESKTOP_CALL_LAUNCH.md) ----

    void headerCallGateFollowsWorkspaceMeetHost()
    {
        Fixture plain;
        QVERIFY(plain.start());
        // No server meet_url: the reference renders no header call action
        // (chat-page-workspace.ui.tsx:4100), so the native gate stays closed.
        QCOMPARE(plain.controller.meetHost(), QString());
        QVERIFY(!plain.controller.canStartCall());

        Fixture configured;
        configured.serverSettings = [](const Request &) { return callMeetSettings(); };
        QVERIFY(configured.start());
        // Origin-only normalization of server_settings.meet_url.
        QTRY_COMPARE(configured.controller.meetHost(), QStringLiteral("https://meet.example.com"));
        QVERIFY(configured.controller.canStartCall());
    }

    void startCallPostsMeetingUrlBeforeOpening()
    {
        Fixture fixture;
        fixture.serverSettings = [](const Request &) { return callMeetSettings(); };
        auto saved = message();
        QJsonObject posted;
        fixture.messages = [&saved, &posted](const Request &request) {
            if (request.method == "POST") {
                posted = request.body;
                saved = message(request.body.value("payload").toObject()
                    .value("content").toString(), returnedId);
                return Response{saved};
            }
            if (request.target.path().endsWith(returnedId))
                return Response{saved, 200, 200};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(fixture.controller.canStartCall());
        fixture.controller.startCall();
        QTRY_VERIFY(fixture.controller.calls()->hasActiveCall());

        QCOMPARE(posted.value("stream_uuid").toString(), streamId);
        QCOMPARE(posted.value("topic_uuid").toString(), topicId);
        const auto payload = posted.value("payload").toObject();
        QCOMPARE(payload.value("kind").toString(), QStringLiteral("markdown"));
        const auto meetingUrl = payload.value("content").toString();
        QVERIFY2(meetingUrl.startsWith(QStringLiteral("https://meet.example.com/workspace-")),
            qPrintable(meetingUrl));
        // workspace-{orgHost}-{project}-{stream}-{topic}-{nowMs}; the mock
        // server host normalizes to 127-0-0-1.
        QVERIFY(meetingUrl.contains(QStringLiteral("workspace-127-0-0-1-")));
        const auto active = fixture.controller.calls()->activeCall();
        QCOMPARE(active.value(QStringLiteral("meetingUrl")).toString(), meetingUrl);
        QCOMPARE(active.value(QStringLiteral("startWithVideoMuted")).toBool(), true);
        QVERIFY(fixture.controller.callError().isEmpty());
    }

    void startCallRefusesDifferentActiveCallBeforeSending()
    {
        Fixture fixture;
        fixture.serverSettings = [](const Request &) { return callMeetSettings(); };
        QVERIFY(fixture.start());
        QTRY_VERIFY(fixture.controller.canStartCall());
        QVariantMap other;
        other.insert(QStringLiteral("meetingUrl"), QStringLiteral("https://meet.example.com/other-room"));
        other.insert(QStringLiteral("locationName"), QStringLiteral("Other"));
        other.insert(QStringLiteral("ownerKey"), fixture.controller.ownerKey());
        fixture.controller.calls()->requestOpenCall(other);
        const int postsBefore = std::count_if(fixture.server.requests.begin(), fixture.server.requests.end(),
            [](const Request &request) { return request.method == "POST" && request.target.path().contains("/messages/"); });

        fixture.controller.startCall();
        QTest::qWait(300);
        QVERIFY(!fixture.controller.callError().isEmpty());
        QCOMPARE(fixture.controller.calls()->activeCall().value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.example.com/other-room"));
        const int postsAfter = std::count_if(fixture.server.requests.begin(), fixture.server.requests.end(),
            [](const Request &request) { return request.method == "POST" && request.target.path().contains("/messages/"); });
        QCOMPARE(postsAfter, postsBefore);
    }

    void startCallNeedsConfirmedSend()
    {
        Fixture fixture;
        fixture.serverSettings = [](const Request &) { return callMeetSettings(); };
        fixture.messages = [](const Request &request) {
            if (request.method == "POST")
                return Response{QJsonObject{}, 500};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(fixture.controller.canStartCall());
        fixture.controller.startCall();
        QTRY_VERIFY(!fixture.controller.callError().isEmpty());
        QVERIFY(!fixture.controller.calls()->hasActiveCall());
    }

    void startCallDoesNotOpenOnUnconfirmedSendBody()
    {
        // A 2xx response only opens the call when it confirms the posted
        // message, same contract as the composer send (review 4555852745).
        Fixture emptyBody;
        emptyBody.serverSettings = [](const Request &) { return callMeetSettings(); };
        emptyBody.messages = [](const Request &request) {
            if (request.method == "POST")
                return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(emptyBody.start());
        QTRY_VERIFY(emptyBody.controller.canStartCall());
        emptyBody.controller.startCall();
        QTRY_VERIFY(!emptyBody.controller.callError().isEmpty());
        QVERIFY(!emptyBody.controller.calls()->hasActiveCall());

        // A well-formed message that is not what we sent confirms nothing.
        Fixture mismatch;
        mismatch.serverSettings = [](const Request &) { return callMeetSettings(); };
        mismatch.messages = [](const Request &request) {
            if (request.method == "POST")
                return Response{message(QStringLiteral("totally different text"), returnedId)};
            return Response{QJsonArray{}};
        };
        QVERIFY(mismatch.start());
        QTRY_VERIFY(mismatch.controller.canStartCall());
        mismatch.controller.startCall();
        QTRY_VERIFY(!mismatch.controller.callError().isEmpty());
        QVERIFY(!mismatch.controller.calls()->hasActiveCall());
    }

    // ---- Issue #163 incoming call invite ingest ----


    static void configureIncomingCall(Fixture &fixture,
        const QString &streamName = QStringLiteral("Slon Direct"),
        bool withCatalogAuthor = true)
    {
        fixture.serverSettings = [](const Request &) { return callMeetSettings(); };
        fixture.streams = [streamName](const Request &) {
            const QJsonObject direct{{QStringLiteral("uuid"), streamId},
                {QStringLiteral("project_id"), projectId}, {QStringLiteral("name"), streamName},
                {QStringLiteral("encryption"), false}, {QStringLiteral("private"), true},
                {QStringLiteral("direct_user_uuid"), channelMemberA},
                {QStringLiteral("unread_count"), 0}, {QStringLiteral("active_unread_count"), 0},
                {QStringLiteral("passive_unread_count"), 0},
                {QStringLiteral("last_message_uuid"), QJsonValue::Null},
                {QStringLiteral("created_at"), QStringLiteral("2026-01-02T10:00:00Z")},
                {QStringLiteral("updated_at"), QStringLiteral("2026-01-02T10:00:00Z")}};
            return Response{QJsonArray{direct}};
        };
        fixture.users = [withCatalogAuthor](const Request &) {
            if (!withCatalogAuthor)
                return Response{QJsonArray{}};
            const QJsonObject self{{QStringLiteral("uuid"), userId},
                {QStringLiteral("username"), QStringLiteral("self")}};
            const QJsonObject author{{QStringLiteral("uuid"), channelMemberA},
                {QStringLiteral("username"), QStringLiteral("slon")},
                {QStringLiteral("first_name"), QStringLiteral("Slon")}};
            return Response{QJsonArray{self, author}};
        };
    }

    static QJsonObject incomingCallMessage(const QString &content)
    {
        auto row = message(content,
            QStringLiteral("dddd0000-0000-4000-8000-000000000001"));
        row[QStringLiteral("author_uuid")] = channelMemberA;
        row[QStringLiteral("is_own")] = false;
        row[QStringLiteral("read")] = false;
        return row;
    }

    void incomingCallRealtimeDirectMessageRaisesInvite()
    {
        Fixture fixture; configureIncomingCall(fixture);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.meetHost(), QStringLiteral("https://meet.example.com"));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created",
            incomingCallMessage(QStringLiteral("https://meet.example.com/workspace-call-room")))));
        QTRY_VERIFY(!fixture.controller.calls()->incomingInvite().isEmpty());
        const auto invite = fixture.controller.calls()->incomingInvite();
        QCOMPARE(invite.value(QStringLiteral("callerName")).toString(),
            QStringLiteral("Slon"));
        QCOMPARE(invite.value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.example.com/workspace-call-room"));
        QCOMPARE(invite.value(QStringLiteral("locationName")).toString(),
            QStringLiteral("Slon Direct"));
        // Accept without the video toggle opens the call with muted video
        // (jitsi-call-shell.ui.tsx:43,61-63).
        fixture.controller.incomingCall()->accept();
        QVERIFY(fixture.controller.calls()->hasActiveCall());
        QCOMPARE(fixture.controller.calls()->activeCall()
            .value(QStringLiteral("startWithVideoMuted")).toBool(), true);
        QVERIFY(fixture.controller.calls()->incomingInvite().isEmpty());
    }

    void incomingCallInviteCallerFallsBackToStreamName()
    {
        // resolveCallerName fallback display name -> stream name (lib.ts:23-34).
        Fixture fixture; configureIncomingCall(fixture, QStringLiteral("Slon Direct"), false);
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created",
            incomingCallMessage(QStringLiteral("https://meet.jit.si/room-x")))));
        QTRY_VERIFY(!fixture.controller.calls()->incomingInvite().isEmpty());
        QCOMPARE(fixture.controller.calls()->incomingInvite()
            .value(QStringLiteral("callerName")).toString(), QStringLiteral("Slon Direct"));
    }

    void incomingCallInviteDeduplicatesSecondDelivery()
    {
        Fixture fixture; configureIncomingCall(fixture);
        QVERIFY(fixture.start());
        const auto row = incomingCallMessage(QStringLiteral("https://meet.example.com/room-d"));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", row)));
        QTRY_VERIFY(!fixture.controller.calls()->incomingInvite().isEmpty());
        // A replay of the same message id changes nothing (model.ts:138-141).
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", row)));
        QCOMPARE(fixture.controller.calls()->incomingInvite()
            .value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.example.com/room-d"));
        fixture.controller.incomingCall()->decline();
        QVERIFY(fixture.controller.calls()->incomingInvite().isEmpty());
        // The remembered id keeps even a post-decline replay suppressed.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", row)));
        QVERIFY(fixture.controller.calls()->incomingInvite().isEmpty());
    }

    void incomingCallInviteIgnoredWhileCallActive()
    {
        Fixture fixture; configureIncomingCall(fixture);
        QVERIFY(fixture.start());
        QVariantMap other;
        other.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.example.com/other-room"));
        other.insert(QStringLiteral("locationName"), QStringLiteral("Other"));
        fixture.controller.calls()->requestOpenCall(other);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created",
            incomingCallMessage(QStringLiteral("https://meet.example.com/room-e")))));
        QTest::qWait(200);
        QVERIFY(fixture.controller.calls()->incomingInvite().isEmpty());
        QCOMPARE(fixture.controller.calls()->activeCall()
            .value(QStringLiteral("meetingUrl")).toString(),
            QStringLiteral("https://meet.example.com/other-room"));
    }

    void incomingCallAcceptUsesVideoTogglePreference()
    {
        Fixture fixture; configureIncomingCall(fixture);
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created",
            incomingCallMessage(QStringLiteral("https://meet.example.com/room-v")))));
        QTRY_VERIFY(!fixture.controller.calls()->incomingInvite().isEmpty());
        const auto messageId = fixture.controller.calls()->incomingInvite()
            .value(QStringLiteral("messageId")).toString();
        fixture.controller.incomingCall()->toggleVideoEnabled(messageId);
        QVERIFY(fixture.controller.incomingCall()->videoEnabled(messageId));
        fixture.controller.incomingCall()->accept();
        QVERIFY(fixture.controller.calls()->hasActiveCall());
        // Video enabled -> the accepted call starts with video unmuted.
        QCOMPARE(fixture.controller.calls()->activeCall()
            .value(QStringLiteral("startWithVideoMuted")).toBool(), false);
    }

    void incomingCallDismissClearsWithoutDeclineOrCall()
    {
        // Escape closes the invite: the invite and its notification clear,
        // nothing declines and no call opens (modal.tsx:59-73).
        Fixture fixture; configureIncomingCall(fixture);
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created",
            incomingCallMessage(QStringLiteral("https://meet.example.com/room-esc")))));
        QTRY_VERIFY(!fixture.controller.calls()->incomingInvite().isEmpty());
        QSignalSpy dismissed(fixture.controller.incomingCall(),
            &IncomingCallLifecycle::dismissRequested);
        fixture.controller.incomingCall()->dismiss();
        QCOMPARE(dismissed.count(), 1);
        QVERIFY(fixture.controller.calls()->incomingInvite().isEmpty());
        QVERIFY(!fixture.controller.calls()->hasActiveCall());
        QVERIFY(!fixture.controller.incomingCall()->visible());
    }

    void joinCallDuringPendingStartIsSerialized()
    {
        // While the start POST is in flight a join click on another room must
        // not open that room: it would leave the posted room blocked-active
        // behind a call the sender cannot open (review 4555852758).
        Fixture fixture;
        fixture.serverSettings = [](const Request &) { return callMeetSettings(); };
        std::function<void()> releasePost;
        QString postedMeetingUrl;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                postedMeetingUrl = request.body.value("payload").toObject()
                    .value("content").toString();
                Response response{message(postedMeetingUrl, returnedId)};
                response.defer = [&releasePost](std::function<void()> send) { releasePost = std::move(send); };
                return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(fixture.controller.canStartCall());
        fixture.controller.startCall();
        QTRY_VERIFY(static_cast<bool>(releasePost)); // The POST is in flight.
        fixture.controller.joinCall(QStringLiteral("https://meet.example.com/rush-room"));
        QTest::qWait(200);
        QVERIFY(!fixture.controller.calls()->hasActiveCall());
        releasePost();
        QTRY_VERIFY(fixture.controller.calls()->hasActiveCall());
        QCOMPARE(fixture.controller.calls()->activeCall().value(QStringLiteral("meetingUrl")).toString(),
            postedMeetingUrl);
        QVERIFY(fixture.controller.callError().isEmpty());
    }

    void encryptedStreamSendsNoPlaintextCallUrl()
    {
        Fixture fixture;
        fixture.encrypted = true;
        fixture.serverSettings = [](const Request &) { return callMeetSettings(); };
        QVERIFY(fixture.start());
        QTRY_VERIFY(fixture.controller.canStartCall());
        fixture.controller.startCall();
        QTest::qWait(300);
        // Recorded deviation: the encrypted path is deferred; nothing may go
        // out as plaintext and the store stays closed.
        QVERIFY(std::none_of(fixture.server.requests.begin(), fixture.server.requests.end(),
            [](const Request &request) { return request.method == "POST" && request.target.path().contains("/messages/"); }));
        QVERIFY(!fixture.controller.callError().isEmpty());
        QVERIFY(!fixture.controller.calls()->hasActiveCall());
    }

    void callUrlRecognitionUsesEffectiveMeetHost()
    {
        Fixture plain;
        QVERIFY(plain.start());
        QCOMPARE(plain.controller.callUrlInText(QStringLiteral("hello")), QString());
        // Without the Workspace setting the build-time host still recognizes.
        QCOMPARE(plain.controller.callUrlInText(QStringLiteral("see https://meet.jit.si/RoomA")),
            QStringLiteral("https://meet.jit.si/RoomA"));

        Fixture configured;
        configured.serverSettings = [](const Request &) { return callMeetSettings(); };
        QVERIFY(configured.start());
        QTRY_VERIFY(!configured.controller.meetHost().isEmpty());
        QCOMPARE(configured.controller.callUrlInText(
                     QStringLiteral("https://meet.example.com/workspace-room-9")),
            QStringLiteral("https://meet.example.com/workspace-room-9"));
        QCOMPARE(configured.controller.callUrlInText(QStringLiteral("https://evil.example/r")), QString());
    }

    // ---- Issue #87 display-capture gate and share-source picker
    // (docs/NATIVE_DESKTOP_CALL_DISPLAY_CAPTURE.md) ----

    static QList<workspace::DisplayCaptureFrame> liveCaptureFrames()
    {
        QList<workspace::DisplayCaptureFrame> frames;
        frames.append({QUrl(QStringLiteral("https://meet.example.com/room")), false});
        return frames;
    }

    void displayCaptureGateDeniesMainFrameRequests()
    {
        const auto frames = liveCaptureFrames();
        // The very origin of the live Jitsi child frame, but flagged as the
        // embedding main frame: the reference handler (main.ts:1003) denies
        // the main frame before any origin check can save it.
        QVERIFY(!workspace::isAllowedDisplayCapture(true,
            QUrl(QStringLiteral("https://meet.example.com/room")), frames));
        // Without any frames at all the main frame still has no path to capture.
        QList<workspace::DisplayCaptureFrame> empty;
        QVERIFY(!workspace::isAllowedDisplayCapture(true,
            QUrl(QStringLiteral("https://meet.example.com/room")), empty));
    }

    void displayCaptureGateDeniesInsecureRequestingOrigins()
    {
        const auto frames = liveCaptureFrames();
        // Same host and path as the live frame, but plain http (main.ts:1009).
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("http://meet.example.com/room")), frames));
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("file:///tmp/page.html")), frames));
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("meet.example.com/room")), frames));
        QVERIFY(!workspace::isAllowedDisplayCapture(false, QUrl(), frames));
    }

    void displayCaptureGateDeniesOriginsOutsideTheLiveFrameSubtree()
    {
        const auto frames = liveCaptureFrames();
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://evil.example/room")), frames));
        // A different port is a different origin in both default-port forms.
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com:8443/room")), frames));
        // No frames at all: nothing can match the requesting origin.
        QList<workspace::DisplayCaptureFrame> empty;
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com/room")), empty));
    }

    void displayCaptureGateDeniesDetachedFrameOrigins()
    {
        QList<workspace::DisplayCaptureFrame> detachedOnly;
        detachedOnly.append({QUrl(QStringLiteral("https://meet.example.com/room")), true});
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com/room")), detachedOnly));
        // A detached match alongside a live unrelated frame still denies.
        QList<workspace::DisplayCaptureFrame> mixed;
        mixed.append({QUrl(QStringLiteral("https://meet.example.com/room")), true});
        mixed.append({QUrl(QStringLiteral("https://other.example/room")), false});
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com/room")), mixed));
    }

    void displayCaptureGateAllowsLiveSecureChildFrameOrigins()
    {
        const auto frames = liveCaptureFrames();
        QVERIFY(workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com/room")), frames));
        // Origin equality, not path equality (main.ts:1015 compares origins).
        QVERIFY(workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com/other/path?q=1")), frames));
        // WHATWG origin form: the default https port drops and the host is
        // case-insensitive on both sides of the comparison.
        QList<workspace::DisplayCaptureFrame> defaultPortFrame;
        defaultPortFrame.append({QUrl(QStringLiteral("https://meet.example.com:443/room")), false});
        QVERIFY(workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://MEET.example.com/room")), defaultPortFrame));
        // A non-default port must be kept on both sides.
        QList<workspace::DisplayCaptureFrame> altPortFrame;
        altPortFrame.append({QUrl(QStringLiteral("https://meet.example.com:8443/room")), false});
        QVERIFY(workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com:8443/r")), altPortFrame));
        QVERIFY(!workspace::isAllowedDisplayCapture(false,
            QUrl(QStringLiteral("https://meet.example.com/r")), altPortFrame));
    }

    void callWindowDisplayCaptureGateGrantsOnlyTheLiveJitsiFrame()
    {
        workspace::JitsiCallWindowController shell;
        QVariantMap call;
        call.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.example.com/workspace-87-room"));
        call.insert(QStringLiteral("meetUrl"), QStringLiteral("https://meet.example.com"));
        shell.open(call);
        // Loading is not live: no frame entry exists before the embed is Ready.
        QVERIFY(!shell.isDisplayCaptureRequestAllowed(false,
            QStringLiteral("https://meet.example.com/room")));
        shell.apiLoadSucceeded();
        QCOMPARE(shell.embedState(), int(workspace::JitsiCallWindowController::Ready));
        QVERIFY(shell.isDisplayCaptureRequestAllowed(false,
            QStringLiteral("https://meet.example.com/room")));
        // Integration negative: a mock main frame cannot obtain capture even
        // when it carries the very meeting origin the live frame holds.
        QVERIFY(!shell.isDisplayCaptureRequestAllowed(true,
            QStringLiteral("https://meet.example.com/room")));
        QVERIFY(!shell.isDisplayCaptureRequestAllowed(false,
            QStringLiteral("http://meet.example.com/room")));
        QVERIFY(!shell.isDisplayCaptureRequestAllowed(false,
            QStringLiteral("https://evil.example/room")));
        // Shell minimize keeps the one live embed alive, so the grant follows.
        shell.toggleMinimize();
        QVERIFY(shell.isDisplayCaptureRequestAllowed(false,
            QStringLiteral("https://meet.example.com/room")));
        // Closing detaches the frame subtree: no request is allowed any more.
        shell.close();
        QVERIFY(!shell.isDisplayCaptureRequestAllowed(false,
            QStringLiteral("https://meet.example.com/room")));
    }

    void callWindowDisplayCaptureGateStaysClosedForInsecureMeetHosts()
    {
        workspace::JitsiCallWindowController shell;
        QVariantMap call;
        call.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("http://meet.example.com/workspace-87-room"));
        call.insert(QStringLiteral("meetUrl"), QStringLiteral("http://meet.example.com"));
        shell.open(call);
        shell.apiLoadSucceeded();
        QCOMPARE(shell.embedState(), int(workspace::JitsiCallWindowController::Ready));
        // The https rule is absolute: a live insecure meeting frame gets no
        // capture just because it is the live frame.
        QVERIFY(!shell.isDisplayCaptureRequestAllowed(false,
            QStringLiteral("http://meet.example.com/room")));
    }

    void shareSourcePickerFallsBackToTheQtScreenModelWithoutPortal()
    {
        workspace::ScreenShareSourcePicker picker;
        picker.setPortalProbe([] { return false; });
        QSignalSpy selected(&picker, &workspace::ScreenShareSourcePicker::sourceSelected);
        QSignalSpy failed(&picker, &workspace::ScreenShareSourcePicker::sourceFailed);
        picker.requestSource();
        QCOMPARE(failed.count(), 0);
        QCOMPARE(selected.count(), 1);
        const auto source = selected.at(0).at(0).toMap();
        QVERIFY(!source.value(QStringLiteral("fromPortal")).toBool());
        QVERIFY(!source.value(QStringLiteral("name")).toString().isEmpty());
        // The fallback publishes exactly Qt's own screen list, so the same
        // call is deterministic on a display without a portal.
        QCOMPARE(int(workspace::ScreenShareSourcePicker::screenModelSources().size()),
            int(QGuiApplication::screens().size()));
        QVERIFY(!workspace::ScreenShareSourcePicker::screenModelSources().isEmpty());
    }

    void shareSourcePickerReportsPortalBridgeResults()
    {
        workspace::ScreenShareSourcePicker picker;
        picker.setPortalProbe([] { return true; });
        QSignalSpy selected(&picker, &workspace::ScreenShareSourcePicker::sourceSelected);
        QSignalSpy failed(&picker, &workspace::ScreenShareSourcePicker::sourceFailed);
        picker.setPortalRequest([](workspace::ScreenShareSourcePicker::Source &source,
            QString &error) {
            Q_UNUSED(error)
            source.id = QStringLiteral("portal-1");
            source.name = QStringLiteral("Whole screen");
            source.uri = QStringLiteral("file:///tmp/lane87-bridge.png");
            source.fromPortal = true;
            return true;
        });
        picker.requestSource();
        QCOMPARE(failed.count(), 0);
        QCOMPARE(selected.count(), 1);
        const auto source = selected.at(0).at(0).toMap();
        QCOMPARE(source.value(QStringLiteral("uri")).toString(),
            QStringLiteral("file:///tmp/lane87-bridge.png"));
        QVERIFY(source.value(QStringLiteral("fromPortal")).toBool());
        picker.setPortalRequest([](workspace::ScreenShareSourcePicker::Source &source,
            QString &error) {
            Q_UNUSED(source)
            error = QStringLiteral("bridge rejected the pick");
            return false;
        });
        picker.requestSource();
        // The earlier success stays published; the failed pick only reports.
        QCOMPARE(selected.count(), 1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("bridge rejected the pick"));
    }

    // Exercise the public ScreenCast wire types and the portal-owned session lifetime.
    void shareSourcePickerDrivesThePortalScreenCastContract()
    {
        auto bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            QSKIP("the session bus is unavailable");
        PortalScreenCastStub cast;
        PortalScreenshotStub shot; // must stay untouched: PictureUri is the wrong contract
        PortalRequestStub shotRequest;
        new PortalRequestAdaptor(&shotRequest);
        QObject portal; // one object owns both portal interfaces at the portal path
        new PortalScreenshotAdaptor(&shot, &portal);
        shot.requestObject = &shotRequest;
        new PortalScreenCastAdaptor(&cast, &portal);
        PortalSessionStub session;
        QVERIFY(bus.registerObject(cast.sessionPath, &session, QDBusConnection::ExportAllSlots));
        PortalRequestPool pool;
        QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &portal));
        QVERIFY(bus.registerObject(shotRequest.path = shot.requestPath, &shotRequest));
        QVERIFY(pool.registerOn(bus,
            QStringLiteral("/org/freedesktop/portal/desktop/request/87/step"), 12, cast));
        if (!bus.registerService(QStringLiteral("org.freedesktop.portal.Desktop")))
            QSKIP("a real portal service owns org.freedesktop.portal.Desktop");
        auto cleanup = qScopeGuard([&] {
            bus.unregisterObject(QStringLiteral("/org/freedesktop/portal/desktop"));
            bus.unregisterObject(shot.requestPath);
            bus.unregisterObject(cast.sessionPath);
            pool.unregisterFrom(bus);
            bus.unregisterService(QStringLiteral("org.freedesktop.portal.Desktop"));
        });
        workspace::ScreenShareSourcePicker picker; // default probe and default transport
        QSignalSpy selected(&picker, &workspace::ScreenShareSourcePicker::sourceSelected);
        QSignalSpy failed(&picker, &workspace::ScreenShareSourcePicker::sourceFailed);
        picker.requestSource();
        QTRY_VERIFY_WITH_TIMEOUT(selected.count() == 1 || failed.count() == 1, 8000);
        QCOMPARE(failed.count(), 0);
        // The live contract ran end to end, and the still-image API was never used.
        QCOMPARE(cast.createSessionCalls, 1);
        QCOMPARE(cast.selectSourcesCalls, 1);
        QCOMPARE(cast.startCalls, 1);
        QVERIFY(cast.validTokens);
        QCOMPARE(cast.selectedSession, cast.sessionPath);
        QCOMPARE(cast.startedSession, cast.sessionPath);
        QCOMPARE(cast.types, 3u);
        QCOMPARE(shot.pictureUriCalls, 0);
        const auto source = selected.at(0).at(0).toMap();
        QCOMPARE(source.value(QStringLiteral("id")).toString(), QStringLiteral("portal-stream"));
        QCOMPARE(source.value("nodeId").toUInt(), cast.nodeId);
        QCOMPARE(source.value("sessionHandle").toString(), cast.sessionPath);
        QCOMPARE(source.value("properties").toMap().value("pipewire-serial").toULongLong(), cast.streamSerial);
        QVERIFY(source.value(QStringLiteral("fromPortal")).toBool());
        QVERIFY(source.value(QStringLiteral("live")).toBool());

        QCOMPARE(session.closeCalls, 0);
        picker.releaseSource();
        QTRY_COMPARE(session.closeCalls, 1);

        cast.startResponseCode = 1;
        picker.requestSource();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 8000);
        QCOMPARE(selected.count(), 1);
        QCOMPARE(cast.startCalls, 2);
        QVERIFY(!failed.at(0).at(0).toString().isEmpty());
        QTRY_COMPARE(session.closeCalls, 2);
    }

    void shareSourcePickerHandlesPortalResponseTiming_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("immediate-alternate-request-path") << QString("immediate");
        QTest::newRow("timeout-during-create") << QString("timeout");
        QTest::newRow("release-during-start") << QString("release");
        QTest::newRow("release-before-alternate-request-reply") << QString("releaseBeforePath");
    }

    void shareSourcePickerHandlesPortalResponseTiming()
    {
        QFETCH(QString, scenario);
        auto bus = QDBusConnection::sessionBus();
        if (!bus.isConnected() || !bus.registerService("org.freedesktop.portal.Desktop"))
            QSKIP("the fixture needs a private session bus");
        auto unown = qScopeGuard([&] { bus.unregisterService("org.freedesktop.portal.Desktop"); });
        QObject portal;
        PortalScreenCastStub cast;
        new PortalScreenCastAdaptor(&cast, &portal);
        const QString portalPath = "/org/freedesktop/portal/desktop";
        QVERIFY(bus.registerObject(portalPath, &portal));
        PortalRequestPool requests;
        QVERIFY(requests.registerOn(bus, portalPath + "/request/fixture/alternative", 3, cast));
        PortalSessionStub session;
        cast.sessionCreated = [&](const QString &token) {
            auto sender = bus.baseService().mid(1);
            cast.sessionPath = portalPath + "/session/" + sender.replace('.', '_') + "/" + token;
            QVERIFY(bus.registerObject(cast.sessionPath, &session, QDBusConnection::ExportAllSlots));
        };
        auto cleanup = qScopeGuard([&] {
            requests.unregisterFrom(bus);
            bus.unregisterObject(cast.sessionPath);
            bus.unregisterObject(portalPath);
        });
        cast.responseDelayMs = scenario == "immediate" ? -1 : 120;
        ScreenShareSourcePicker picker;
        picker.setPortalTimeoutMs(scenario == "timeout" ? 60 : 1000);
        QSignalSpy selected(&picker, &ScreenShareSourcePicker::sourceSelected);
        QSignalSpy failed(&picker, &ScreenShareSourcePicker::sourceFailed);
        picker.requestSource();
        if (scenario == "immediate") {
            QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 1800);
            QCOMPARE(failed.size(), 0);
            picker.releaseSource();
        } else {
            if (scenario.startsWith("release")) {
                if (scenario == "release")
                    QTRY_COMPARE_WITH_TIMEOUT(cast.startCalls, 1, 800);
                picker.releaseSource();
            }
            QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1800);
            QTRY_VERIFY(cast.requestCursor > 0);
            QTRY_COMPARE(cast.requestPool.at(cast.requestCursor - 1)->closeCalls, 1);
            QTest::qWait(150);
            QCOMPARE(selected.size(), 0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(session.closeCalls, 1, 1000);
    }

    void shareSourcePickerRejectsMalformedOrMissingPortalStreams()
    {
        QVERIFY(ScreenShareSourcePicker::streamFromResult({}).isEmpty());
        QVERIFY(ScreenShareSourcePicker::streamFromResult({{"streams", "/not/a/node"}}).isEmpty());
        QVERIFY(ScreenShareSourcePicker::streamFromResult({{"streams", QStringList{"/not/a/node"}}}).isEmpty());
    }

    // #87 round 2 (finding 3): a completed exchange's timeout timer and
    // Request subscription must be cancelled at completion, so a stale timer
    // or a late Response can never fail or fake a later exchange. The picker
    // timeout (330 ms) is armed per exchange while the stub answers the
    // first exchange (200 ms) before it and the second one (400 ms) after
    // it: before the fix the first exchange's singleShot fires inside the
    // second exchange and emits a bogus timeout failure for it.
    void shareSourcePickerCancelsTheCompletedExchangeTimerAndSubscription()
    {
        auto bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            QSKIP("the session bus is unavailable");
        PortalScreenshotStub shot;
        shot.responseDelayMs = 200;
        PortalRequestStub shotRequest;
        new PortalRequestAdaptor(&shotRequest);
        QObject portal; // one object owns both portal interfaces at the portal path
        new PortalScreenshotAdaptor(&shot, &portal);
        shot.requestObject = &shotRequest;
        PortalScreenCastStub cast;
        cast.responseDelayMs = 60;
        new PortalScreenCastAdaptor(&cast, &portal);
        PortalRequestPool pool;
        QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &portal));
        QVERIFY(bus.registerObject(shotRequest.path = shot.requestPath, &shotRequest));
        QVERIFY(pool.registerOn(bus,
            QStringLiteral("/org/freedesktop/portal/desktop/request/87/timer"), 12, cast));
        if (!bus.registerService(QStringLiteral("org.freedesktop.portal.Desktop")))
            QSKIP("a real portal service owns org.freedesktop.portal.Desktop");
        auto cleanup = qScopeGuard([&] {
            bus.unregisterObject(QStringLiteral("/org/freedesktop/portal/desktop"));
            bus.unregisterObject(shot.requestPath);
            pool.unregisterFrom(bus);
            bus.unregisterService(QStringLiteral("org.freedesktop.portal.Desktop"));
        });
        workspace::ScreenShareSourcePicker picker; // default probe and default transport
        picker.setPortalTimeoutMs(330);
        QSignalSpy selected(&picker, &workspace::ScreenShareSourcePicker::sourceSelected);
        QSignalSpy failed(&picker, &workspace::ScreenShareSourcePicker::sourceFailed);
        picker.requestSource();
        QTRY_VERIFY_WITH_TIMEOUT(selected.count() == 1 || failed.count() == 1, 8000);
        QCOMPARE(failed.count(), 0); // the first exchange completes cleanly
        picker.requestSource();      // and hands straight over to a second one
        QTRY_VERIFY_WITH_TIMEOUT(selected.count() == 2 || failed.count() == 1, 8000);
        // Only the second exchange's own portal reply may decide its outcome.
        QCOMPARE(failed.count(), 0);
        QCOMPARE(selected.count(), 2);
    }

    // #87 round 2 (findings 1/2): the shipped embed surface decides display
    // capture through the controller's frame-trust gate. Qt 6.10.2 delivers
    // the request as a QWebEnginePermission exposing only origin and
    // permissionType (qwebenginepermission.h:30-33, 70-71) - no frame object,
    // no main-frame flag, no frame tree - so the requesting frame can never
    // be verified as the live meeting child frame the reference gate demands
    // (workspace_ui main.ts:996-1020 denies unless that is established).
    // Reproduction of findings 1/2: with the shipped wiring (the view
    // asserting isMainFrame=false) the very meeting origin passes the modeled
    // child-frame check even though a main-frame request is indistinguishable
    // from it, so this must fail closed.
    void callWindowDisplayCaptureGateFailsClosedWithoutFrameIdentity()
    {
        workspace::JitsiCallWindowController shell;
        QVariantMap call;
        call.insert(QStringLiteral("meetingUrl"),
            QStringLiteral("https://meet.example.com/workspace-87-room"));
        call.insert(QStringLiteral("meetUrl"), QStringLiteral("https://meet.example.com"));
        shell.open(call);
        shell.apiLoadSucceeded();
        QCOMPARE(shell.embedState(), int(workspace::JitsiCallWindowController::Ready));
        // A matching origin is NOT verifiable frame identity: the meeting
        // document is this view's main frame, so a grant would be exactly the
        // main-frame grant the finding condemns.
        QVERIFY(!shell.isDisplayCaptureRequestTrustworthy(
            QStringLiteral("https://meet.example.com/workspace-87-room")));
        QVERIFY(!shell.isDisplayCaptureRequestTrustworthy(
            QStringLiteral("https://evil.example/room")));
        shell.close();
        QVERIFY(!shell.isDisplayCaptureRequestTrustworthy(
            QStringLiteral("https://meet.example.com/workspace-87-room")));
    }

    // ---- Issue #164 DM call bridge (docs/NATIVE_DESKTOP_DM_CALL_BRIDGE.md) ----

    static int callMessagePosts(Fixture &fixture)
    {
        return int(std::count_if(fixture.server.requests.begin(), fixture.server.requests.end(),
            [](const Request &request) { return request.method == "POST" && request.target.path().contains("/messages/"); }));
    }

    static void deferCallInvites(Fixture &fixture, QVector<QJsonObject> &saved,
        QVector<std::function<void()>> &release)
    {
        fixture.messages = [&saved, &release](const Request &request) {
            if (request.method == "POST") {
                const auto body = request.body;
                const auto topic = body.value("topic_uuid").toString();
                auto row = message(body.value("payload").toObject().value("content").toString(),
                    QStringLiteral("ca110000-0000-4000-8000-%1").arg(saved.size() + 1, 12, 10, QChar('0')), topic);
                row["stream_uuid"] = body.value("stream_uuid").toString();
                saved.append(row);
                Response response{row};
                response.defer = [&release](std::function<void()> go) { release.append(std::move(go)); };
                return response;
            }
            return Response{QJsonArray{}};
        };
    }

    // Fixture::start asserts a single-stream catalog; call-bridge tests that
    // navigate between two channels log in with this multi-stream variant.
    static bool startMultiStream(Fixture &fixture, int streamCount)
    {
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user",
            "synthetic-password", "");
        if (!QTest::qWaitFor([&] { return fixture.controller.projects().size() == 1; }, 2000))
            return false;
        fixture.controller.openProject(projectId);
        if (!QTest::qWaitFor([&] { return fixture.controller.streams().size() == streamCount; }, 2000))
            return false;
        fixture.controller.selectStream(streamId);
        if (!QTest::qWaitFor([&] { return !fixture.controller.topics().isEmpty(); }, 2000))
            return false;
        fixture.controller.selectTopic(topicId);
        return QTest::qWaitFor([&] { return !fixture.controller.busy(); }, 2000);
    }

    // Store contract port of chat-dm-call-bridge.model.test.ts:10-35 plus the
    // no-op-clear, consume-once and three-field descriptor-match rules.
    void dmCallBridgeStoreContract()
    {
        workspace::DmCallBridge bridge;
        QSignalSpy pendingSpy(&bridge, &workspace::DmCallBridge::pendingPartnerUuidChanged);
        QSignalSpy surfaceSpy(&bridge, &workspace::DmCallBridge::callSurfaceEnabledChanged);
        QSignalSpy invokeSpy(&bridge, &workspace::DmCallBridge::callSurfaceInvokeRequested);
        QCOMPARE(bridge.pendingPartnerUuid(), QString());
        // clear on empty is a no-op: no write, no notify (:50-51).
        bridge.clearPendingPartner();
        QCOMPARE(pendingSpy.count(), 0);
        QCOMPARE(bridge.consumePendingPartner(), QString());
        QCOMPARE(pendingSpy.count(), 0);
        bridge.setPendingPartnerUserUuid(QStringLiteral("partner-a"));
        QCOMPARE(bridge.pendingPartnerUuid(), QStringLiteral("partner-a"));
        QCOMPARE(pendingSpy.count(), 1);
        bridge.clearPendingPartner();
        QCOMPARE(bridge.pendingPartnerUuid(), QString());
        QCOMPARE(pendingSpy.count(), 2);
        // A consumed pending clears exactly once and never re-fires.
        bridge.setPendingPartnerUserUuid(QStringLiteral("partner-b"));
        QCOMPARE(pendingSpy.count(), 3);
        QCOMPARE(bridge.consumePendingPartner(), QStringLiteral("partner-b"));
        QCOMPARE(bridge.pendingPartnerUuid(), QString());
        QCOMPARE(pendingSpy.count(), 4);
        QCOMPARE(bridge.consumePendingPartner(), QString());
        QCOMPARE(pendingSpy.count(), 4);
        // Descriptor gating: enabled only on the full owner+generation+stream match.
        bridge.setViewedChat(QStringLiteral("owner"), 7, QStringLiteral("stream-a"), true);
        QVERIFY(bridge.viewedChatIsDirect());
        QVERIFY(!bridge.callSurfaceEnabled());
        bridge.invokeCallSurface();
        QCOMPARE(invokeSpy.count(), 0); // the reference hands out no invoke at all
        const QVariantMap full{{QStringLiteral("ownerKey"), QStringLiteral("owner")},
            {QStringLiteral("runtimeGeneration"), qulonglong(7)}, {QStringLiteral("streamUuid"), QStringLiteral("stream-a")}};
        bridge.setActiveCallDescriptor(full);
        QVERIFY(bridge.callSurfaceEnabled());
        QCOMPARE(surfaceSpy.count(), 1);
        bridge.setActiveCallDescriptor(QVariantMap{{QStringLiteral("ownerKey"), QStringLiteral("other")},
            {QStringLiteral("runtimeGeneration"), qulonglong(7)}, {QStringLiteral("streamUuid"), QStringLiteral("stream-a")}});
        QVERIFY(!bridge.callSurfaceEnabled());
        bridge.setActiveCallDescriptor(QVariantMap{{QStringLiteral("ownerKey"), QStringLiteral("owner")},
            {QStringLiteral("runtimeGeneration"), qulonglong(8)}, {QStringLiteral("streamUuid"), QStringLiteral("stream-a")}});
        QVERIFY(!bridge.callSurfaceEnabled());
        bridge.setActiveCallDescriptor(QVariantMap{{QStringLiteral("ownerKey"), QStringLiteral("owner")},
            {QStringLiteral("runtimeGeneration"), qulonglong(7)}, {QStringLiteral("streamUuid"), QStringLiteral("stream-b")}});
        QVERIFY(!bridge.callSurfaceEnabled());
        bridge.invokeCallSurface();
        QCOMPARE(invokeSpy.count(), 0); // still mismatched
        bridge.setActiveCallDescriptor(full);
        QVERIFY(bridge.callSurfaceEnabled());
        bridge.setViewedChat(QStringLiteral("owner"), 7, QStringLiteral("stream-a"), false);
        QVERIFY(!bridge.viewedChatIsDirect()); // viewed identity change keeps the match
        QVERIFY(bridge.callSurfaceEnabled());
        bridge.setViewedChat(QStringLiteral("owner"), 9, QStringLiteral("stream-a"), true); // generation bumped
        QVERIFY(!bridge.callSurfaceEnabled());
        bridge.setActiveCallDescriptor(full);
        QVERIFY(!bridge.callSurfaceEnabled()); // full descriptor against the newer generation
        bridge.clearActiveCallDescriptor();
        QVERIFY(bridge.activeCallDescriptor().isEmpty());
        QVERIFY(!bridge.callSurfaceEnabled());
        bridge.invokeCallSurface();
        QCOMPARE(invokeSpy.count(), 0);
        bridge.setViewedChat(QStringLiteral("owner"), 7, QStringLiteral("stream-a"), true);
        bridge.setActiveCallDescriptor(full);
        QVERIFY(bridge.callSurfaceEnabled());
        bridge.invokeCallSurface();
        QCOMPARE(invokeSpy.count(), 1); // exactly one routed invoke
        QCOMPARE(surfaceSpy.count(), 5); // every true<->false transition fired NOTIFY
    }

    // Profile Call with no existing DM: create the DM, then send exactly one
    // invite into the default topic and open the call once the send is applied.
    void dmCallProfileCreatesDmThenOpensCallOnce()
    {
        DirectChatFixture test;
        test.fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        QVector<QJsonObject> saved; QVector<std::function<void()>> release;
        deferCallInvites(test.fixture, saved, release);
        QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        auto *bridge = controller.callBridge();
        QVERIFY(bridge);
        QVERIFY(bridge->pendingPartnerUuid().isEmpty());
        controller.callFromProfile(channelMemberA);
        // The pending partner is queued up front (right-panel-user-profile.ui.tsx:491).
        QCOMPARE(bridge->pendingPartnerUuid(), channelMemberA);
        QVERIFY(controller.profileCallPending()); // callPending chip gate
        QTRY_VERIFY(!controller.profileCallPending());
        // The DM landed with its default topic and the handoff consumed the pending.
        QTRY_COMPARE(controller.selectedStream(), expectedDirectId(channelMemberA));
        QTRY_COMPARE(controller.selectedTopic(), channelDefault);
        QTRY_COMPARE(bridge->pendingPartnerUuid(), QString());
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 1; }, 2000));
        QCOMPARE(callMessagePosts(test.fixture), 1); // exactly one invite
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved[0].value("topic_uuid").toString(), channelDefault);
        QVERIFY(saved[0].value("payload").toObject().value("content").toString()
            .startsWith(QStringLiteral("https://meet.example.com/workspace-")));
        release[0](); // the invite send is confirmed
        QTRY_VERIFY(controller.calls()->hasActiveCall());
        QCOMPARE(controller.calls()->activeCall().value(QStringLiteral("meetingUrl")).toString(),
            saved[0].value("payload").toObject().value("content").toString());
        QVERIFY(controller.callError().isEmpty());
        for (auto &go : release) go(); // drain deferred deliveries for teardown
    }

    // A failed create clears the pending partner and leaves nothing open
    // (right-panel-user-profile.ui.tsx:507, :513).
    void dmCallProfileFailedCreateClearsPendingAndOpensNothing()
    {
        DirectChatFixture test;
        test.applyPost = false;
        test.postStatus = 400;
        test.fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        controller.callFromProfile(channelMemberA);
        QCOMPARE(controller.callBridge()->pendingPartnerUuid(), channelMemberA); // set before the create
        QVERIFY(controller.profileCallPending());
        QTRY_VERIFY(!controller.profileCallPending());
        QTRY_COMPARE(controller.callBridge()->pendingPartnerUuid(), QString()); // cleared on failure
        QCOMPARE(callMessagePosts(test.fixture), 0);
        QVERIFY(!controller.calls()->hasActiveCall());
        QCOMPARE(controller.selectedStream(), streamId); // no navigation happened
    }

    // Already-open DM: the handoff fires on the spot, exactly once, and never
    // re-fires while the same DM stays open.
    void dmCallProfileAlreadyOpenHandoffFiresExactlyOnce()
    {
        Fixture fixture;
        fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        const auto dmId = expectedDirectId(channelMemberA);
        fixture.streams = [dmId](const Request &request) {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/" + dmId)
                return Response{creationDirect(channelMemberA, dmId)};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}}};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}, creationDirect(channelMemberA, dmId)}};
        };
        fixture.topics = [dmId](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == dmId) {
                auto topic = creationTopic(channelDefault, dmId, "General Topic");
                topic["is_default"] = true;
                return Response{QJsonArray{topic}};
            }
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        QVector<QJsonObject> saved; QVector<std::function<void()>> release;
        deferCallInvites(fixture, saved, release);
        QVERIFY(startMultiStream(fixture, 2));
        auto &controller = fixture.controller;
        controller.selectStream(dmId);
        QVERIFY(QTest::qWaitFor([&] { return !controller.topics().isEmpty(); }, 2000));
        controller.selectTopic(channelDefault);
        QVERIFY(QTest::qWaitFor([&] { return !controller.busy(); }, 2000));
        controller.callFromProfile(channelMemberA);
        // Already open: stay put, and the invite lands in the viewed topic.
        QCOMPARE(controller.selectedStream(), dmId);
        QCOMPARE(controller.selectedTopic(), channelDefault);
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 1; }, 2000));
        QCOMPARE(controller.callBridge()->pendingPartnerUuid(), QString());
        QCOMPARE(callMessagePosts(fixture), 1);
        QCOMPARE(saved.first().value("topic_uuid").toString(), channelDefault);
        release[0](); // confirm the single invite
        QTRY_VERIFY(controller.calls()->hasActiveCall());
        // The same DM stays open: re-entering the viewed conversation must not
        // re-fire the handoff (the consumed pending is the guard).
        controller.selectStream(dmId);
        QVERIFY(QTest::qWaitFor([&] { return !controller.busy(); }, 2000));
        QTest::qWait(300);
        QCOMPARE(callMessagePosts(fixture), 1);
        for (auto &go : release) go();
    }

    // Active-call descriptor: registered for the viewed chat, the surface is
    // enabled only on the full three-field match, and header + sidebar share one
    // in-flight guard (double invoke -> exactly one invite).
    void dmCallDescriptorMatchesViewedChatAndSharesGuard()
    {
        static const QString secondStream = QStringLiteral("d1d1d1d1-1111-4111-8111-111111111111");
        Fixture fixture;
        fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        fixture.streams = [](const Request &request) {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/" + secondStream)
                return Response{creationChannel(secondStream, QStringLiteral("Second channel"))};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}}};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}},
                creationChannel(secondStream, QStringLiteral("Second channel"))}};
        };
        QVector<QJsonObject> saved; QVector<std::function<void()>> release;
        deferCallInvites(fixture, saved, release);
        QVERIFY(startMultiStream(fixture, 2));
        auto &controller = fixture.controller;
        auto *bridge = controller.callBridge();
        // Registered for the viewed chat: the sidebar surface is enabled.
        QTRY_COMPARE(bridge->activeCallDescriptor().value(QStringLiteral("streamUuid")).toString(), streamId);
        QCOMPARE(bridge->activeCallDescriptor().value(QStringLiteral("ownerKey")).toString(), controller.creationOwnerKey());
        QVERIFY(bridge->callSurfaceEnabled());
        const auto full = bridge->activeCallDescriptor();
        // Each single-field mismatch disables the surface (right-panel-workspace-info
        // selector port; behaviour test right-panel-workspace-info.ui.test.tsx:333-370).
        bridge->setActiveCallDescriptor(QVariantMap{{QStringLiteral("ownerKey"), QStringLiteral("other-owner")},
            {QStringLiteral("runtimeGeneration"), full.value(QStringLiteral("runtimeGeneration"))},
            {QStringLiteral("streamUuid"), streamId}});
        QVERIFY(!bridge->callSurfaceEnabled());
        bridge->setActiveCallDescriptor(QVariantMap{{QStringLiteral("ownerKey"), controller.creationOwnerKey()},
            {QStringLiteral("runtimeGeneration"), full.value(QStringLiteral("runtimeGeneration")).toULongLong() + 1},
            {QStringLiteral("streamUuid"), streamId}});
        QVERIFY(!bridge->callSurfaceEnabled());
        bridge->setActiveCallDescriptor(QVariantMap{{QStringLiteral("ownerKey"), controller.creationOwnerKey()},
            {QStringLiteral("runtimeGeneration"), full.value(QStringLiteral("runtimeGeneration"))},
            {QStringLiteral("streamUuid"), secondStream}});
        QVERIFY(!bridge->callSurfaceEnabled());
        bridge->invokeCallSurface();
        QTest::qWait(200);
        QCOMPARE(release.size(), 0); // a mismatched surface is not invocable
        bridge->setActiveCallDescriptor(full);
        QVERIFY(bridge->callSurfaceEnabled());
        // Header and sidebar share one guard: double invoke -> one invite.
        bridge->invokeCallSurface();
        controller.startCall();
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 1; }, 2000));
        controller.startCall(); // the pending send owns the guard
        QTest::qWait(200);
        QCOMPARE(release.size(), 1);
        QCOMPARE(callMessagePosts(fixture), 1);
        for (auto &go : release) go();
        QTRY_VERIFY(controller.calls()->hasActiveCall());
        QVERIFY(controller.callError().isEmpty());
    }

    // A->B->A navigation: the old send resolving after the round trip must not
    // open the call and must not release the newer invoke's guard
    // (chat-page-workspace.test.tsx:5597-5665).
    void dmCallStaleCompletionAfterBackNavigationOpensNothing()
    {
        static const QString secondStream = QStringLiteral("d1d1d1d1-1111-4111-8111-111111111111");
        static const QString secondStreamTopic = QStringLiteral("d2d2d2d2-2222-4222-8222-222222222222");
        Fixture fixture;
        fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        fixture.streams = [](const Request &request) {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/" + secondStream)
                return Response{creationChannel(secondStream, QStringLiteral("Second channel"))};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}}};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}},
                creationChannel(secondStream, QStringLiteral("Second channel"))}};
        };
        fixture.topics = [secondStream, secondStreamTopic](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == secondStream)
                return Response{QJsonArray{creationTopic(secondStreamTopic, secondStream, QStringLiteral("Second topic"))}};
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        QVector<QJsonObject> saved; QVector<std::function<void()>> release;
        deferCallInvites(fixture, saved, release);
        QVERIFY(startMultiStream(fixture, 2));
        auto &controller = fixture.controller;
        auto *bridge = controller.callBridge();
        controller.startCall(); // send 1 from conversation A
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 1; }, 2000));
        controller.selectStream(secondStream); // A -> B
        QVERIFY(QTest::qWaitFor([&] { return !controller.busy(); }, 2000));
        controller.selectStream(streamId); // B -> A
        QVERIFY(QTest::qWaitFor([&] { return !controller.topics().isEmpty(); }, 2000));
        controller.selectTopic(topicId);
        QVERIFY(QTest::qWaitFor([&] { return !controller.busy(); }, 2000));
        bridge->invokeCallSurface(); // the same route again: a fresh send is allowed
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 2; }, 2000));
        release[0](); // the old completion lands
        QTest::qWait(300);
        QVERIFY(!controller.calls()->hasActiveCall()); // no late Jitsi window
        QVERIFY(controller.callError().isEmpty()); // stale completions are silent
        bridge->invokeCallSurface(); // the newer send still owns the guard
        QTest::qWait(200);
        QCOMPARE(release.size(), 2);
        release[1]();
        QTRY_VERIFY(controller.calls()->hasActiveCall());
        QCOMPARE(controller.calls()->activeCall().value(QStringLiteral("meetingUrl")).toString(),
            saved[1].value("payload").toObject().value("content").toString());
    }

    // A session (runtime generation) change aborts an in-flight invite: the
    // transport cancels it and the completion opens nothing.
    void dmCallSessionChangeAbortsInFlightInvite()
    {
        Fixture fixture;
        fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        QVector<QJsonObject> saved; QVector<std::function<void()>> release;
        deferCallInvites(fixture, saved, release);
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        QTRY_VERIFY(controller.callBridge()->callSurfaceEnabled());
        controller.startCall();
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 1; }, 2000));
        controller.logout();
        QVERIFY(QTest::qWaitFor([&] { return !controller.authenticated(); }, 2000));
        QTest::qWait(300); // the transport cancels the in-flight send
        QVERIFY(!controller.calls()->hasActiveCall());
        QVERIFY(controller.callBridge()->activeCallDescriptor().isEmpty());
        QVERIFY(controller.callError().isEmpty());
        // A late server answer (if the mock ever delivers) changes nothing;
        // the cancelled request completed inside the transport already.
    }

    // E2EE stream without an available session: the invoke is refused with the
    // cryptoUnavailable error and nothing is sent (reference :5328-5341).
    void dmCallEncryptedWithoutSessionRefusedWithCryptoUnavailable()
    {
        Fixture fixture;
        fixture.encrypted = true;
        fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        QTRY_VERIFY(controller.canStartCall());
        // No descriptor for an encrypted stream without a local key: the
        // sidebar call surface stays disabled (encryptedChatAvailable gate).
        QVERIFY(controller.callBridge()->activeCallDescriptor().isEmpty());
        QVERIFY(!controller.callBridge()->callSurfaceEnabled());
        controller.startCall();
        QTRY_COMPARE(controller.callError(), QStringLiteral(
            "Encryption is unavailable for this chat or local key storage could not be opened."));
        QCOMPARE(callMessagePosts(fixture), 0);
        QVERIFY(!controller.calls()->hasActiveCall());
    }

    // Review finding 1: when a direct chat cannot even be created, the pending
    // partner must not stay queued. A later visit to that peer's DM would
    // otherwise consume the stale entry and fire a call nobody asked for.
    void dmCallProfileCreateUnavailableQueuesNothingAndLaterVisitStaysQuiet()
    {
        static const QString ghostPeer = QStringLiteral("8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e");
        static const QString ghostDm = expectedDirectId(ghostPeer);
        Fixture fixture;
        bool ghostExists = false;
        fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        fixture.streams = [&ghostExists, ghostDm](const Request &request) {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/" + ghostDm)
                return Response{creationDirect(ghostPeer, ghostDm)};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}}};
            QJsonArray rows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}}};
            if (ghostExists) rows.append(creationDirect(ghostPeer, ghostDm));
            return Response{rows};
        };
        fixture.topics = [ghostDm](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == ghostDm) {
                auto topic = creationTopic(channelDefault, ghostDm, "General Topic");
                topic["is_default"] = true;
                return Response{QJsonArray{topic}};
            }
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        QVector<QJsonObject> saved; QVector<std::function<void()>> release;
        deferCallInvites(fixture, saved, release);
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        // The ghost peer has no DM stream and no directory entry: the call chat
        // cannot start, so nothing may be queued for it.
        QVERIFY(!controller.canStartDirectChatWith(ghostPeer));
        controller.callFromProfile(ghostPeer);
        QCOMPARE(controller.callBridge()->pendingPartnerUuid(), QString());
        // A DM for that peer appears later (created elsewhere); visiting it
        // must not fire a call that nobody requested.
        ghostExists = true;
        controller.refresh();
        QVERIFY(QTest::qWaitFor([&] { return controller.streams().size() == 2; }, 2000));
        controller.selectStream(ghostDm);
        QVERIFY(QTest::qWaitFor([&] { return !controller.topics().isEmpty(); }, 2000));
        controller.selectTopic(channelDefault);
        QVERIFY(QTest::qWaitFor([&] { return !controller.busy(); }, 2000));
        QTest::qWait(300);
        QCOMPARE(callMessagePosts(fixture), 0);
        QVERIFY(!controller.calls()->hasActiveCall());
        QVERIFY(controller.callBridge()->pendingPartnerUuid().isEmpty());
    }

    // Review finding 2: a same-owner session generation change regenerates the
    // transport generation. The captured call identity must include it, so the
    // cancelled invite completion lands silently and the new session starts
    // with a free call lane.
    void dmCallSameOwnerGenerationChangeInvalidatesInFlightCallIdentity()
    {
        Fixture fixture;
        fixture.serverSettings = [](const Request &) {
            return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
                {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
        };
        QVector<QJsonObject> saved; QVector<std::function<void()>> release;
        deferCallInvites(fixture, saved, release);
        QVERIFY(fixture.start());
        auto &controller = fixture.controller;
        QTRY_VERIFY(controller.callBridge()->callSurfaceEnabled());
        controller.startCall();
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 1; }, 2000));
        const auto generation = fixture.api.generation();
        fixture.api.setAccessToken("same-owner-regenerated-session");
        QVERIFY(fixture.api.generation() > generation);
        QVERIFY(controller.authenticated()); // same owner: only the generation moved
        QTest::qWait(300); // the transport cancels the in-flight send
        QVERIFY(controller.callError().isEmpty()); // the aborted completion stays silent
        QVERIFY(!controller.calls()->hasActiveCall());
        // The call lane is free again: a fresh send in the new session posts
        // exactly one invite, is guarded, and opens once confirmed.
        controller.startCall();
        QVERIFY(QTest::qWaitFor([&] { return release.size() >= 2; }, 2000));
        controller.startCall(); // the pending send owns the guard
        QTest::qWait(200);
        QCOMPARE(release.size(), 2);
        release[1]();
        QTRY_VERIFY(controller.calls()->hasActiveCall());
        QVERIFY(controller.callError().isEmpty());
        for (auto &go : release) go();
    }

    // Review finding 3: the profile Call chip's encryption-availability
    // dependency must be notifying: callCryptoStateChanged fires on every
    // relevant stream or key state transition (catalog rows, viewed chat,
    // local key lifetime), not only when the boolean answer itself flips.
    void dmCallCryptoAvailabilityNotifyTransitions()
    {
        static const QString secondStream = QStringLiteral("d1d1d1d1-1111-4111-8111-111111111111");
        static const QString secondStreamTopic = QStringLiteral("d2d2d2d2-2222-4222-8222-222222222222");
        Fixture fixture;
        fixture.streams = [](const Request &request) {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/" + secondStream)
                return Response{creationChannel(secondStream, QStringLiteral("Second channel"))};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}}};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}},
                creationChannel(secondStream, QStringLiteral("Second channel"))}};
        };
        fixture.topics = [secondStream, secondStreamTopic](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == secondStream)
                return Response{QJsonArray{creationTopic(secondStreamTopic, secondStream, QStringLiteral("Second topic"))}};
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        QVERIFY(startMultiStream(fixture, 2));
        auto &controller = fixture.controller;
        QSignalSpy crypto(&controller, SIGNAL(callCryptoStateChanged()));
        QVERIFY2(crypto.isValid(), "the controller must expose callCryptoStateChanged");
        QVERIFY(!controller.property("callCryptoSessionReady").toBool()); // no vault attached
        // Stream catalog reload: the peer DM encryption rows were re-read.
        crypto.clear();
        controller.refresh();
        QTRY_VERIFY(!controller.busy());
        QVERIFY2(crypto.count() >= 1, "a stream catalog reload must notify call crypto availability");
        // Viewed conversation change: the local-key part of the answer moves
        // with the viewed stream.
        crypto.clear();
        controller.selectStream(secondStream);
        QVERIFY(QTest::qWaitFor([&] { return !controller.topics().isEmpty(); }, 2000));
        controller.selectTopic(secondStreamTopic);
        QVERIFY(QTest::qWaitFor([&] { return !controller.busy(); }, 2000));
        QVERIFY2(crypto.count() >= 1, "a viewed conversation change must notify call crypto availability");
        // Local key lifetime teardown: signing out wipes the chat runtime.
        crypto.clear();
        controller.logout();
        QTRY_VERIFY(!controller.authenticated());
        QVERIFY2(crypto.count() >= 1, "chat key teardown must notify call crypto availability");
    }
};

QTEST_MAIN(ControllerCallsTest)
#include "native-controller-calls-test.moc"
