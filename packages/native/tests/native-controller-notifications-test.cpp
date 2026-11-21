#include "power-watcher.h"
#include "native-call-awareness.h"
#include "native-desktop.h"
#include "support/native-test-harness.h"
#include "support/controller-realtime-fixtures.h"
#include "support/controller-notifications-fixtures.h"

// UPower-shaped stub for the power fixture bus: the adaptor backs
// org.freedesktop.DBus.Properties.Get, so the watcher's initial read sees the
// battery exactly like the real daemon answers it.
class UpowerStub final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    bool onBattery = false;
    // Holds the Properties.Get reply inside the adaptor read long enough for a
    // test to emit a fresher PropertiesChanged, reproducing a delayed stale
    // reply arriving after the newer signal (fix-round-1 finding 2).
    int getDelayMs = 0;
};

class UpowerAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.UPower")
    Q_PROPERTY(bool OnBattery READ onBattery)
public:
    explicit UpowerAdaptor(UpowerStub *owner) : QDBusAbstractAdaptor(owner), m_owner(owner) {}
    bool onBattery() const
    {
        if (m_owner->getDelayMs > 0) {
            // The fixture's "daemon side" dispatches this read on the shared
            // thread; keep pumping events so the bus keeps moving while the
            // reply is held.
            QDeadlineTimer deadline(m_owner->getDelayMs);
            while (!deadline.hasExpired())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return m_owner->onBattery;
    }
private:
    UpowerStub *m_owner;
};

// Mirrors CallAwareness's private InhibitCallTimeoutMs (native-call-awareness.cpp)
// so the slow-service cases can sit exactly past the client-side deadline.
const int InhibitCallTimeoutMs = 2000;

// Display-sleep inhibitor stubs for the call-awareness cases (issue #86),
// exported on the private bus below exactly like the real freedesktop
// services answer Inhibit/UnInhibit on the session bus.
class ScreenSaverStub final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    QList<QPair<QString, QString>> inhibits; // (application, reason)
    QList<uint> uninhibits;
    uint cookie = 1701;
    int inhibitDelayMs = 0; // held daemon-side reply for the slow-service cases
};

class ScreenSaverAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    explicit ScreenSaverAdaptor(ScreenSaverStub *owner) : QDBusAbstractAdaptor(owner), m_owner(owner) {}
public slots:
    uint Inhibit(const QString &application, const QString &reason)
    {
        m_owner->inhibits.append(qMakePair(application, reason));
        if (m_owner->inhibitDelayMs > 0) {
            // The fixture's "daemon side" dispatches this call on the shared
            // thread; keep pumping events so the bus and the client's own
            // deadline timers keep moving while the reply is held
            // (the UpowerAdaptor precedent).
            QDeadlineTimer deadline(m_owner->inhibitDelayMs);
            while (!deadline.hasExpired())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return m_owner->cookie;
    }
    void UnInhibit(uint cookie) { m_owner->uninhibits.append(cookie); }
private:
    ScreenSaverStub *m_owner;
};

class PmInhibitStub final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    QList<QPair<QString, QString>> inhibits; // (application, reason)
    QList<uint> uninhibits;
    uint cookie = 4242;
};

class PmInhibitAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.PowerManagement.Inhibit")
public:
    explicit PmInhibitAdaptor(PmInhibitStub *owner) : QDBusAbstractAdaptor(owner), m_owner(owner) {}
public slots:
    uint Inhibit(const QString &application, const QString &reason)
    {
        m_owner->inhibits.append(qMakePair(application, reason));
        return m_owner->cookie;
    }
    void Uninhibit(uint cookie) { m_owner->uninhibits.append(cookie); }
private:
    PmInhibitStub *m_owner;
};

// Private session bus for the power watcher (issue #78) so the tests never
// touch the host's real logind/UPower. Two named connections: the watched
// bus handed to the watcher, and a "daemon side" that owns the well-known
// names and emits the signals, like the real daemons do.
class PowerBusFixture final
{
public:
    QProcess daemon;
    // QDBusConnection has no default ctor; sessionBus() is a per-process
    // handle even when disconnected, replaced by the fixture bus below.
    QDBusConnection watcherBus {QDBusConnection::sessionBus()};
    QDBusConnection daemonSide {QDBusConnection::sessionBus()};
    bool ok = false;
    explicit PowerBusFixture(const QString &tag)
    {
        static int counter = 0;
        const auto id = QString::number(++counter);
        const auto exe = QStandardPaths::findExecutable("dbus-daemon");
        if (exe.isEmpty()) return;
        daemon.setProgram(exe);
        daemon.setArguments({"--session", "--print-address=1", "--nofork"});
        daemon.start();
        if (!daemon.waitForStarted(5000) || !daemon.waitForReadyRead(5000)) return;
        const auto address = QString::fromLatin1(daemon.readLine()).trimmed();
        if (!address.startsWith("unix:")) return;
        m_watcherName = "power-watch-" + tag + '-' + id;
        m_daemonName = "power-daemon-" + tag + '-' + id;
        watcherBus = QDBusConnection::connectToBus(address, m_watcherName);
        daemonSide = QDBusConnection::connectToBus(address, m_daemonName);
        ok = watcherBus.isConnected() && daemonSide.isConnected();
    }
    ~PowerBusFixture()
    {
        if (!m_watcherName.isEmpty()) QDBusConnection::disconnectFromBus(m_watcherName);
        if (!m_daemonName.isEmpty()) QDBusConnection::disconnectFromBus(m_daemonName);
        if (daemon.state() != QProcess::NotRunning) {
            daemon.terminate();
            if (!daemon.waitForFinished(3000)) daemon.kill();
            daemon.waitForFinished(2000);
        }
    }
    PowerBusFixture(const PowerBusFixture &) = delete;
    PowerBusFixture &operator=(const PowerBusFixture &) = delete;
    bool claimService(const QString &name) { return daemonSide.registerService(name); }
    bool exportObject(const QString &path, QObject *object)
    {
        return daemonSide.registerObject(path, object);
    }
    void emitPrepareForSleep(bool sleeping)
    {
        auto signal = QDBusMessage::createSignal("/org/freedesktop/login1",
            "org.freedesktop.login1.Manager", "PrepareForSleep");
        signal << sleeping;
        daemonSide.send(signal);
    }
    void emitOnBattery(bool onBattery)
    {
        auto signal = QDBusMessage::createSignal("/org/freedesktop/UPower",
            "org.freedesktop.DBus.Properties", "PropertiesChanged");
        signal << QVariant(QStringLiteral("org.freedesktop.UPower"))
            << QVariant(QVariantMap{{QStringLiteral("OnBattery"), onBattery}})
            << QVariant(QStringList());
        daemonSide.send(signal);
    }
private:
    QString m_watcherName;
    QString m_daemonName;
};

class ControllerNotificationsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- desktop notifications (issue #3, docs/NATIVE_DESKTOP_NOTIFICATIONS.md) ----

    // The fixture auto-opens its first conversation; notification candidates
    // must therefore target a background conversation. Open the second topic
    // so messages in topicId are off-screen candidates everywhere below.
    // Detail endpoints must serve parse-valid DTOs for the route to resolve.
    void openBackgroundConversation(Fixture &fixture)
    {
        Q_UNUSED(fixture);
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == secondTopicId, 2000);
    }

    void notificationPolicyMatrix_data()
    {
        QTest::addColumn<QString>("row");
        QTest::addColumn<bool>("notify");
        QTest::addColumn<QString>("trigger");
        QTest::newRow("dm") << "dm" << true << QStringLiteral("dm");
        QTest::newRow("own") << "own" << false << QString();
        QTest::newRow("read") << "read" << false << QString();
        QTest::newRow("mention-default-mode") << "mention-default" << true << QStringLiteral("mention");
        QTest::newRow("mention-mentions-only") << "mention-mentions-only" << true << QStringLiteral("mention");
        QTest::newRow("wildcard") << "wildcard" << true << QStringLiteral("wildcard_mention");
        QTest::newRow("followed-topic") << "followed-topic" << true << QStringLiteral("followed_topic");
        QTest::newRow("plain-stream-default-mode") << "plain-stream" << false << QStringLiteral("stream");
        QTest::newRow("plain-stream-all-messages") << "plain-stream-all" << true << QStringLiteral("stream");
        QTest::newRow("stream-muted") << "stream-muted" << false << QString();
        QTest::newRow("stream-muted-mention") << "stream-muted-mention" << false << QString();
        QTest::newRow("topic-mute-wins-over-stream-all") << "topic-mute" << false << QString();
        QTest::newRow("follow-unmutes-muted-stream") << "follow-unmute" << true << QStringLiteral("followed_topic");
        QTest::newRow("focused-on-screen") << "focused-on-screen" << false << QString();
        QTest::newRow("focused-off-screen") << "focused-off-screen" << true << QStringLiteral("stream");
    }
    void notificationPolicyMatrix()
    {
        QFETCH(QString, row);
        QFETCH(bool, notify);
        QFETCH(QString, trigger);
        NotificationCandidate c;
        c.owner = QStringLiteral("owner");
        c.messageUuid = QStringLiteral("m1");
        c.streamUuid = streamId;
        c.topicUuid = topicId;
        c.authorUuid = notificationAuthor;
        c.content = QStringLiteral("Ping");
        c.streamMode = QStringLiteral("default");
        c.topicMode = QStringLiteral("default");
        if (row == "dm") c.streamPrivate = true;
        else if (row == "own") c.isOwn = true;
        else if (row == "read") c.read = true;
        else if (row == "mention-default") c.mentioned = true;
        else if (row == "mention-mentions-only") { c.mentioned = true; c.streamMode = QStringLiteral("mentions_only"); }
        else if (row == "wildcard") c.content = QStringLiteral("standup @all tomorrow");
        else if (row == "followed-topic") c.topicMode = QStringLiteral("follow");
        else if (row == "plain-stream-all") c.streamMode = QStringLiteral("all_messages");
        else if (row == "stream-muted") c.streamMode = QStringLiteral("muted");
        else if (row == "stream-muted-mention") { c.streamMode = QStringLiteral("muted"); c.mentioned = true; }
        else if (row == "topic-mute") { c.streamMode = QStringLiteral("all_messages"); c.topicMode = QStringLiteral("mute"); }
        else if (row == "follow-unmute") { c.streamMode = QStringLiteral("muted"); c.topicMode = QStringLiteral("follow"); }
        else if (row == "focused-on-screen") { c.viewport.windowFocused = true; c.viewport.conversationOnScreen = true; }
        else if (row == "focused-off-screen") { c.streamMode = QStringLiteral("all_messages"); c.viewport.windowFocused = true; }
        const auto decision = notification_policy::shouldNotify(c, false);
        QCOMPARE(decision.notify, notify);
        if (!trigger.isEmpty()) QCOMPARE(decision.trigger, trigger);
    }

    void notificationTitleFormatsFollowTheReference()
    {
        const QString mid = QString::fromUtf8(" \xC2\xB7 ");
        NotificationTitleContext dm;
        dm.direct = true;
        dm.senderName = QStringLiteral("X");
        dm.conversationName = QStringLiteral("Personal notes");
        // Count wording is delegated to Qt plural catalogs (the locale guard
        // verifies the catalog); the structural contract is count + place.
        QCOMPARE(NotificationService::formatTitle(dm, 1), QStringLiteral("X"));
        QCOMPARE(NotificationService::formatTitle(dm, 2),
            QStringLiteral("2 messages") + mid + QStringLiteral("Personal notes"));
        NotificationTitleContext channel;
        channel.senderName = QStringLiteral("X");
        channel.channelName = QStringLiteral("Native channel");
        channel.topicName = QStringLiteral("Native topic");
        QCOMPARE(NotificationService::formatTitle(channel, 2),
            QStringLiteral("2 messages from X") + mid + QStringLiteral("Native channel")
                + mid + QStringLiteral("Native topic"));
        channel.topicName.clear();
        QCOMPARE(NotificationService::formatTitle(channel, 1), QStringLiteral("X") + mid + QStringLiteral("Native channel"));
        // Electron caps notification text at 200 characters (main.ts).
        NotificationTitleContext huge;
        huge.channelName = QString(300, QChar('x'));
        QVERIFY(NotificationService::formatTitle(huge, 1).size() <= 200);
    }

    void twoBackgroundMessagesFromOneAuthorShareOneTag()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        fixture.controller.notifications()->setTransport(&sent);
        openBackgroundConversation(fixture);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated",
            QJsonObject{{"uuid", notificationAuthor}, {"project_id", projectId},
                        {"username", "author-x"}, {"first_name", "X"},
                        {"updated_at", "2026-01-02T10:00:00Z"}}, 12)));
        const auto first = incomingDto(historyMessage(500), notificationAuthor);
        auto second = incomingDto(historyMessage(501), notificationAuthor);
        const auto target = QStringLiteral("urn:secret:v1:%1:%2:YWJj").arg(streamId, messageId);
        second["payload"] = QJsonObject{{"kind", "markdown"},
            {"content", QStringLiteral("Message 501: [private label](%1)").arg(target)}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 13)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", second, 14)));
        const QString mid = QString::fromUtf8(" \xC2\xB7 ");
        const QString place = QStringLiteral("Native channel") + mid + QStringLiteral("Native topic");
        QCOMPARE(sent.shows.size(), 2);
        QCOMPARE(sent.shows[0].title, QStringLiteral("X") + mid + place);
        QCOMPARE(sent.shows[0].body, QStringLiteral("Message 500"));
        QCOMPARE(sent.shows[1].title, QStringLiteral("2 messages from X") + mid + place);
        QCOMPARE(sent.shows[1].body, QStringLiteral("Message 501: Encrypted fragment"));
        QCOMPARE(sent.shows[1].tag, sent.shows[0].tag);
        // Re-delivering the same event changes nothing (processed marker + dedup).
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 15)));
        QCOMPARE(sent.shows.size(), 2);
        // Same author, a different background topic: separate bucket, separate
        // tag. (secondTopicId is the visible conversation here, so the message
        // must target a third topic to stay an off-screen candidate.)
        auto otherTopic = incomingDto(historyMessage(502), notificationAuthor);
        otherTopic["topic_uuid"] = QStringLiteral("33333333-3333-4333-8333-333333333333");
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, "Second topic"),
                notificationTopicDto(QStringLiteral("33333333-3333-4333-8333-333333333333"),
                    QStringLiteral("Third topic"))}};
        };
        fixture.controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.topics().size() == 3, 4000);
        QCOMPARE(fixture.controller.selectedTopic(), secondTopicId);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", otherTopic, 16)));
        QCOMPARE(sent.shows.size(), 3);
        QVERIFY(sent.shows[2].tag != sent.shows[0].tag);
        fixture.controller.notifications()->setTransport(nullptr);
    }

    void readingTheNewestNotificationMessageClosesTheTag()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        const auto first = incomingDto(historyMessage(500), notificationAuthor);
        const auto second = incomingDto(historyMessage(501), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 13)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", second, 14)));
        const auto tag = sent.lastTag();
        QCOMPARE(service->aggregateCount(tag), 2);
        // Reading the older message keeps the tag and shrinks the count.
        auto readOlder = first;
        readOlder["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readOlder, 15)));
        QVERIFY(!sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 1);
        // Reading the newest message closes the whole tag.
        auto readNewest = second;
        readNewest["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readNewest, 16)));
        QVERIFY(sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 0);
        // A read for a message nobody notified about closes its fallback tag
        // (untracked uuids fall back to the msg: scheme in the reference).
        auto stray = incomingDto(historyMessage(503), notificationAuthor);
        auto strayRead = stray;
        strayRead["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", strayRead, 17)));
        const auto owner = tag.section(QStringLiteral("::"), 0, 0).mid(7); // strip "bucket:"
        QVERIFY(sent.closed.contains(
            NotificationAggregateRegistry::fallbackTag(owner, stray.value(QStringLiteral("uuid")).toString())));
        QCOMPARE(service->aggregateCount(tag), 0);
        service->setTransport(nullptr);
    }

    void readingBulkReadEventsClosesNotificationTags()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        const auto only = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", only, 13)));
        const auto tag = sent.lastTag();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read",
            QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{only.value("uuid").toString()}}}, 14)));
        QVERIFY(sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 0);
        service->setTransport(nullptr);
    }

    void focusedConversationOnScreenSuppressesNotifications()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, "Second topic")}};
        };
        QVERIFY(fixture.start());
        fixture.controller.notifications()->setTransport(&sent);
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == secondTopicId, 2000);
        fixture.controller.setWindowFocused(true);
        // The open conversation never notifies, even for fresh unread arrivals.
        auto inView = incomingDto(historyMessage(500), notificationAuthor);
        inView["topic_uuid"] = secondTopicId;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", inView, 13)));
        QVERIFY(sent.shows.isEmpty());
        // A different conversation does notify while focused, without attention.
        auto off = incomingDto(historyMessage(501), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", off, 14)));
        QCOMPARE(sent.shows.size(), 1);
        QCOMPARE(sent.attentionRequests, 0);
        fixture.controller.notifications()->setTransport(nullptr);
    }

    void genericNotificationFiresAfterTheMetadataGrace()
    {
        Fixture fixture;
        RecordingNotifications sent;
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(120, 40);
        // The candidate stream is not in the loaded catalog at all, so
        // metadata cannot resolve and the generic fallback must fire.
        auto stray = incomingDto(historyMessage(600), notificationAuthor);
        stray["stream_uuid"] = QStringLiteral("9e9e9e9e-9e9e-4e9e-8e9e-9e9e9e9e9e9e");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", stray, 13)));
        QVERIFY(sent.shows.isEmpty()); // inside the grace window
        QTRY_VERIFY_WITH_TIMEOUT(sent.shows.size() == 1, 3000);
        QCOMPARE(sent.shows.first().title, QStringLiteral("You have a new message"));
        QVERIFY(!sent.shows.first().body.contains(QStringLiteral("://")));
        QCOMPARE(sent.shows.first().body, QString::fromUtf8("synthetic-user \xC2\xB7 127.0.0.1"));
        QTRY_VERIFY(sent.shows.size() == 1);
        service->setTransport(nullptr);
    }

    void attentionRequestFollowsWindowFocus()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        fixture.controller.setWindowFocused(false);
        auto first = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 13)));
        QCOMPARE(sent.shows.size(), 1);
        QCOMPARE(sent.attentionRequests, 1);
        QCOMPARE(sent.attentionClears, 0);
        // Gaining focus clears the indication once; a focused delivery asks no more.
        fixture.controller.setWindowFocused(true);
        QCOMPARE(sent.attentionClears, 1);
        auto second = incomingDto(historyMessage(501), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", second, 14)));
        QCOMPARE(sent.shows.size(), 2);
        QCOMPARE(sent.attentionRequests, 1);
        service->setTransport(nullptr);
    }

    void notificationClickOpensTheConversationAndRetiresTheTag()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        auto arrival = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 13)));
        QCOMPARE(sent.shows.size(), 1);
        const auto tag = sent.lastTag();
        service->click(tag);
        QVERIFY(sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedStream() == streamId
            && fixture.controller.selectedTopic() == topicId, 3000);
        // Clicking a dead tag does nothing further.
        service->click(tag);
        QCOMPARE(sent.shows.size(), 1);
        service->setTransport(nullptr);
    }

    void notificationClickRequestsWindowActivation()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        QSignalSpy activations(service, &NotificationService::activationRequested);
        QVERIFY(activations.isValid());
        auto arrival = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 13)));
        QCOMPARE(sent.shows.size(), 1);
        // Delivering a banner must not activate the window by itself.
        QCOMPARE(activations.count(), 0);
        const auto tag = sent.lastTag();
        // A valid aggregate click requests activation, then routes the conversation.
        service->click(tag);
        QCOMPARE(activations.count(), 1);
        QVERIFY(sent.closed.contains(tag));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedStream() == streamId
            && fixture.controller.selectedTopic() == topicId, 3000);
        // A retired tag never requests activation again.
        service->click(tag);
        QCOMPARE(activations.count(), 1);
        // An unknown tag stays a full no-op.
        service->click(QStringLiteral("tag:never-shown"));
        QCOMPARE(activations.count(), 1);
        service->setTransport(nullptr);
    }

    void fallbackNotificationClickRequestsWindowActivation()
    {
        Fixture fixture;
        RecordingNotifications sent;
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(120, 40);
        QSignalSpy activations(service, &NotificationService::activationRequested);
        QSignalSpy routes(service, &NotificationService::routeRequested);
        QVERIFY(activations.isValid());
        QVERIFY(routes.isValid());
        // The candidate stream is outside the loaded catalog, so the generic
        // msg:-scheme banner is the only live tag for this message.
        auto stray = incomingDto(historyMessage(600), notificationAuthor);
        stray["stream_uuid"] = QStringLiteral("9e9e9e9e-9e9e-4e9e-8e9e-9e9e9e9e9e9e");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", stray, 13)));
        QTRY_VERIFY_WITH_TIMEOUT(sent.shows.size() == 1, 3000);
        QCOMPARE(activations.count(), 0);
        const auto tag = sent.lastTag();
        // A valid fallback click also requests activation before routing.
        service->click(tag);
        QCOMPARE(activations.count(), 1);
        QCOMPARE(routes.count(), 1);
        QVERIFY(sent.closed.contains(tag));
        // The consumed fallback tag is dead: a second click changes nothing.
        service->click(tag);
        QCOMPARE(activations.count(), 1);
        QCOMPARE(routes.count(), 1);
        service->setTransport(nullptr);
    }

    void dbusNotifyCallMatchesTheFreedesktopSignature()
    {
        auto bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            QSKIP("the session bus is unavailable");
        FakeFreedesktopNotifications daemon;
        new FakeFreedesktopNotificationsAdaptor(&daemon);
        if (!bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &daemon))
            QSKIP("cannot export the fake notification service");
        if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications")))
            QSKIP("a real notification daemon owns org.freedesktop.Notifications");
        workspace::DBusNotificationTransport transport([] { return quintptr{}; });
        QVERIFY(transport.show(QStringLiteral("tag:signature"), QStringLiteral("Title text"),
            QStringLiteral("Body text")));
        QCOMPARE(daemon.notifyCalls.size(), 1);
        const auto &call = daemon.notifyCalls.first();
        QCOMPARE(call.summary, QStringLiteral("Title text"));
        QCOMPARE(call.body, QStringLiteral("Body text"));
        QCOMPARE(call.replacesId, 0u);
        // Click routing only survives daemons that do not synthesize the
        // implicit default action when the client declares it explicitly.
        QVERIFY(call.actions.contains(QStringLiteral("default")));
        transport.closeByTag(QStringLiteral("tag:signature"));
        QTRY_VERIFY_WITH_TIMEOUT(daemon.closedIds.size() == 1, 2000);
        bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
        bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
    }

    void mutedBackgroundTopicSilencesNotifications()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.counters = [](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/stream_topics/" + secondTopicId)
                return Response{notificationTopicDto(secondTopicId,
                    QStringLiteral("Quiet topic"), QStringLiteral("mute"))};
            if (path == "/api/workspace/v1/messenger/stream_topics/" + topicId)
                return Response{notificationTopicDto()};
            return Response{notificationStreamDto()};
        };
        QVERIFY(fixture.start()); // the visible conversation is topicId
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(2000, 150);
        auto arrival = incomingDto(historyMessage(500), notificationAuthor);
        // Background topic outside the loaded catalog: the mute state is only
        // visible through the detail endpoint, which the resolver must consult.
        arrival["topic_uuid"] = secondTopicId;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 13)));
        const auto catalogSize = fixture.controller.topics().size();
        QTest::qWait(2600); // beyond the grace window a misjudged candidate would have shown
        QVERIFY(sent.shows.isEmpty());
        // The backfilled topic must never reshape the active catalog.
        QCOMPARE(fixture.controller.topics().size(), catalogSize);
        service->setTransport(nullptr);
    }

    // --- issue #78: power state (docs/NATIVE_DESKTOP_POWER_STATE.md) ---

    void powerResumeReconnectsRealtimeWithoutBackoff()
    {
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10},
                    {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), QStringLiteral("live"));
        QCOMPARE(gateway.sockets.size(), 1);
        PowerBusFixture bus("resume");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        QVERIFY(bus.claimService("org.freedesktop.login1"));
        workspace::PowerWatcher watcher;
        fixture.controller.setPowerWatcher(&watcher);
        watcher.start(bus.watcherBus);
        // PrepareForSleep(true) is informational: it must not tear down the
        // healthy link.
        bus.emitPrepareForSleep(true);
        QTest::qWait(300);
        QCOMPARE(gateway.sockets.size(), 1);
        QCOMPARE(fixture.controller.syncState(), QStringLiteral("live"));
        // PrepareForSleep(false): a healthy link has no other reason to drop
        // and come back, so the gateway's second socket can only be the
        // resume-triggered reconnect, and it must arrive without the backoff.
        bus.emitPrepareForSleep(false);
        QTRY_COMPARE_WITH_TIMEOUT(gateway.sockets.size(), 2, 3000);
        QTRY_COMPARE(fixture.controller.syncState(), QStringLiteral("live"));
    }

    void batteryCadenceFollowsUpowerInitialReadAndPropertyChanges()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.isOnBattery(), false);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 30000);
        PowerBusFixture bus("battery");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        UpowerStub upower;
        upower.onBattery = true;
        new UpowerAdaptor(&upower);
        QVERIFY(bus.exportObject("/org/freedesktop/UPower", &upower));
        QVERIFY(bus.claimService("org.freedesktop.UPower"));
        workspace::PowerWatcher watcher;
        fixture.controller.setPowerWatcher(&watcher);
        watcher.start(bus.watcherBus);
        // The initial Properties.Get already reports the battery: 30000*2.
        QTRY_VERIFY(watcher.isOnBattery());
        QTRY_COMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 60000);
        QVERIFY(fixture.controller.isOnBattery());
        // Unfocused keeps the reference pair 160000 on battery / 120000 on AC.
        fixture.controller.setWindowFocused(false);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 160000);
        bus.emitOnBattery(false);
        QTRY_COMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 120000);
        fixture.controller.setWindowFocused(true);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 30000);
        bus.emitOnBattery(true);
        QTRY_VERIFY(fixture.controller.isOnBattery());
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 60000);
        bus.emitOnBattery(false);
        QTRY_VERIFY(!fixture.controller.isOnBattery());
    }

    void powerWatcherDegradesOnABusWithoutPowerServices()
    {
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10},
                    {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), QStringLiteral("live"));
        PowerBusFixture bus("bare");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        workspace::PowerWatcher watcher;
        fixture.controller.setPowerWatcher(&watcher);
        watcher.start(bus.watcherBus);
        // A bus without logind or UPower: the initial Get fails quietly, no
        // signal ever arrives, the mains cadence stays, the healthy realtime
        // link survives the tick, and nothing crashed.
        QTest::qWait(500);
        QVERIFY(!watcher.isOnBattery());
        QVERIFY(!fixture.controller.isOnBattery());
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 30000);
        QCOMPARE(gateway.sockets.size(), 1);
        QCOMPARE(fixture.controller.syncState(), QStringLiteral("live"));
    }

    void powerWatcherDefaultsToTheSystemBus()
    {
        // Fix-round-1 finding 1 (cid 4576526925): logind and UPower are
        // system-bus services, so the default connection a start() with no
        // argument selects must be the system bus; the injectable-bus
        // parameter stays for the private fixture bus the other cases use.
        QVERIFY(workspace::PowerWatcher::defaultBusKind() == workspace::PowerWatcher::Bus::System);
        // The seam is what start()'s default argument uses, and it resolves
        // to the process' system-bus handle (same bus object, not just a bus
        // that happens to be reachable).
        QVERIFY(workspace::PowerWatcher::defaultBus().interface()
            == QDBusConnection::systemBus().interface());
    }

    void staleInitialBatteryReadYieldsToFreshPowerSignal()
    {
        // Fix-round-1 finding 2 (cid 4576526932): a Properties.Get reply that
        // lands after a fresher PropertiesChanged must not overwrite it.
        Fixture fixture;
        QVERIFY(fixture.start());
        PowerBusFixture bus("stale-read");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        UpowerStub upower;
        upower.onBattery = true;
        upower.getDelayMs = 700; // The reply lands after the signal below.
        new UpowerAdaptor(&upower);
        QVERIFY(bus.exportObject("/org/freedesktop/UPower", &upower));
        QVERIFY(bus.claimService("org.freedesktop.UPower"));
        workspace::PowerWatcher watcher;
        fixture.controller.setPowerWatcher(&watcher);
        watcher.start(bus.watcherBus);
        // The mains signal arrives while the stale read is still in flight;
        // when that reply finally lands it must be discarded.
        bus.emitOnBattery(false);
        QTest::qWait(1100); // Past the held reply and the signal both.
        QVERIFY(!watcher.isOnBattery());
        QVERIFY(!fixture.controller.isOnBattery());
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 30000);
        // A real change after the discarded reply still reaches the client.
        bus.emitOnBattery(true);
        QTRY_VERIFY(watcher.isOnBattery());
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 60000);
    }

    void hiddenWindowUsesReferenceHiddenPresenceCadence()
    {
        // Fix-round-1 finding 3 (cid 4576526934): a hidden window must use the
        // hidden tier. Reference: workspacePresenceIntervalMs returns
        // max(baseIntervalMs, WORKSPACE_PRESENCE_HIDDEN_INTERVAL_MS) * batteryFactor
        // for the hidden state (user-workspace-presence-reporter.lib.ts:39 at
        // 61dacef7), with WORKSPACE_PRESENCE_HIDDEN_INTERVAL_MS = 300 000
        // (line 24) and WORKSPACE_PRESENCE_BATTERY_MULTIPLIER = 2 (line 25) —
        // 300000 ms mains, 600000 ms on battery.
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 30000);
        fixture.controller.setWindowVisible(false);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 300000);
        // Hidden outranks focus state and stretches on battery.
        fixture.controller.setWindowFocused(false);
        fixture.controller.setOnBattery(true);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 600000);
        // Coming back restores the focused/unfocused ladder.
        fixture.controller.setWindowVisible(true);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 160000); // unfocused, battery
        fixture.controller.setOnBattery(false);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 120000); // unfocused, mains
        fixture.controller.setWindowFocused(true);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 30000);
    }

    void minimizedWindowUsesReferenceHiddenPresenceCadence()
    {
        // Fix-round-2 finding (cid 4576526934): Qt keeps isVisible() true
        // for a minimized window, so the R1 bool mirror left minimized
        // windows on the unfocused tier. The reference ladder keys on
        // document.hidden, and in the reference (Chromium-backed) shell a
        // minimized window is hidden: WORKSPACE_PRESENCE_HIDDEN_INTERVAL_MS
        // = 300 000 (user-workspace-presence-reporter.lib.ts:24 at
        // 61dacef7), WORKSPACE_PRESENCE_BATTERY_MULTIPLIER = 2 (line 25),
        // and the hidden branch returns max(base, 300 000) * batteryFactor
        // (line 39). Minimized must land on 300000 mains / 600000 battery.
        Fixture fixture;
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 30000);
        // A minimized window: Qt reports it visible-but-unfocused, exactly
        // what main.cpp's visibilityChanged wiring forwards.
        fixture.controller.setWindowFocused(false);
        fixture.controller.setWindowMinimized(true);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 300000);
        fixture.controller.setOnBattery(true);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 600000);
        // Restoring the window returns to the unfocused ladder...
        fixture.controller.setWindowMinimized(false);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 160000);
        // ...and a hidden window still selects the hidden tier by itself.
        fixture.controller.setWindowFocused(true);
        fixture.controller.setWindowVisible(false);
        QCOMPARE(fixture.controller.presenceHeartbeatIntervalMs(), 600000);
    }

    void spoofedResumeBurstCannotSpamRealtimeReconnects()
    {
        // Fix-round-1 finding 4 (cid 4576526936): on an injected bus any peer
        // may own org.freedesktop.login1, so repeated PrepareForSleep(false)
        // must not pile up backoff-bypassing reconnects beyond the resume
        // reconnect floor.
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10},
                    {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), QStringLiteral("live"));
        QCOMPARE(gateway.sockets.size(), 1);
        PowerBusFixture bus("resume-burst");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        QVERIFY(bus.claimService("org.freedesktop.login1"));
        workspace::PowerWatcher watcher;
        fixture.controller.setPowerWatcher(&watcher);
        watcher.start(bus.watcherBus);
        // The first resume reconnects immediately, like a real wake.
        bus.emitPrepareForSleep(false);
        QTRY_COMPARE_WITH_TIMEOUT(gateway.sockets.size(), 2, 3000);
        // A burst inside the floor window cannot force further reconnects.
        // The short gaps let every forced reconnect reach the gateway accept,
        // so an unbounded spam would visibly pile up sockets here.
        for (int i = 0; i < 5; ++i) {
            bus.emitPrepareForSleep(false);
            QTest::qWait(200);
        }
        QTest::qWait(500);
        QCOMPARE(gateway.sockets.size(), 2);
        QCOMPARE(fixture.controller.syncState(), QStringLiteral("live"));
    }

    void genuineSleepCycleResumeBypassesReconnectFloor()
    {
        // Fix-round-2 finding (cid 4576829140): monotonic elapsed() excludes
        // suspended time, so a genuine sleep->wake->sleep->wake can leave the
        // second resume inside the five second floor and wrongly suppress it.
        // A resume that was PRECEDED by a PrepareForSleep(true) sleep
        // notification is a real sleep cycle and always reconnects; only bare
        // resumes without a preceding sleep notification stay floored (the
        // spoof/burst pattern from fix-round-1 finding 4).
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10},
                    {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), QStringLiteral("live"));
        QCOMPARE(gateway.sockets.size(), 1);
        PowerBusFixture bus("sleep-cycle");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        QVERIFY(bus.claimService("org.freedesktop.login1"));
        workspace::PowerWatcher watcher;
        fixture.controller.setPowerWatcher(&watcher);
        watcher.start(bus.watcherBus);
        // Bare storm first: the first resume reconnects, the rest are floored,
        // so the baseline stays at the two legitimate sockets.
        bus.emitPrepareForSleep(false);
        QTRY_COMPARE_WITH_TIMEOUT(gateway.sockets.size(), 2, 3000);
        for (int i = 0; i < 3; ++i) {
            bus.emitPrepareForSleep(false);
            QTest::qWait(150);
        }
        QTest::qWait(300);
        QCOMPARE(gateway.sockets.size(), 2);
        // Genuine sleep cycles inside the same floor window: every resume
        // follows a PrepareForSleep(true), so every one reconnects every time.
        for (int i = 0; i < 3; ++i) {
            bus.emitPrepareForSleep(true);
            QTest::qWait(100);
            bus.emitPrepareForSleep(false);
            QTRY_COMPARE_WITH_TIMEOUT(gateway.sockets.size(), 3 + i, 3000);
        }
        // The sleep mark is consumed per cycle: a bare burst afterwards is
        // floored again and adds no sockets.
        for (int i = 0; i < 2; ++i) {
            bus.emitPrepareForSleep(false);
            QTest::qWait(150);
        }
        QTest::qWait(300);
        QCOMPARE(gateway.sockets.size(), 5);
        QCOMPARE(fixture.controller.syncState(), QStringLiteral("live"));
    }

    void signOutClosesActiveNotifications()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        auto arrival = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 13)));
        QCOMPARE(sent.shows.size(), 1);
        const auto tag = sent.lastTag();
        fixture.controller.logout();
        // Banners must not outlive the account that produced them.
        QVERIFY(sent.closed.contains(tag));
        service->setTransport(nullptr);
    }

    void notificationStateStaysBounded()
    {
        Fixture fixture;
        RecordingNotifications sent;
        QVERIFY(fixture.start()); // visible topicId suppresses candidates, decisions recorded
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        for (int i = 0; i < 300; ++i) {
            const auto dto = incomingDto(historyMessage(600 + i), notificationAuthor);
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", dto, 1000 + i)));
        }
        QCOMPARE(sent.shows.size(), 0);
        QVERIFY(service->processedCount() <= 2048);
        // Unknown-stream candidates hold on the grace window: that store is
        // bounded too (a hostile sender cannot exhaust client memory).
        service->setMetadataGrace(60000, 50);
        for (int i = 0; i < 100; ++i) {
            auto dto = incomingDto(historyMessage(1600 + i), notificationAuthor);
            dto["stream_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999999");
            dto["topic_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999999");
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", dto, 5000 + i)));
        }
        QVERIFY(service->pendingCount() <= 64);
        service->setTransport(nullptr);
    }

    void aggregateEvictionClosesTheEvictedBanner()
    {
        // The aggregate store is bounded; an evicted bucket must not leave its
        // already-shown desktop banner open untracked forever.
        RecordingNotifications sent;
        NotificationService service;
        service.setTransport(&sent);
        for (int i = 0; i < 132; ++i) {
            NotificationCandidate candidate;
            candidate.owner = QStringLiteral("owner-eviction");
            candidate.messageUuid =
                QStringLiteral("aaaa0000-0000-4000-8000-%1").arg(i, 12, 10, QLatin1Char('0'));
            candidate.streamUuid = QStringLiteral("stream-evict");
            candidate.topicUuid = QStringLiteral("topic-evict");
            candidate.authorUuid = candidate.messageUuid;
            candidate.content = QStringLiteral("Eviction probe %1").arg(i);
            candidate.senderName = QStringLiteral("Author %1").arg(i);
            candidate.topicName = QStringLiteral("Evicting");
            candidate.streamMode = QStringLiteral("all_messages");
            candidate.topicMode = QStringLiteral("default");
            candidate.streamKnown = true;
            candidate.topicKnown = true;
            candidate.createdMicros = 480LL * 1000000 + i;
            service.ingest(candidate);
        }
        QVERIFY(sent.shows.size() >= 129);
        QVERIFY2(!sent.closed.isEmpty(),
                 "eviction must tag-close the evicted desktop banner");
    }

    void aggregateEvictionKeepsTheNewestTiedRow()
    {
        // Eviction must never drop the message the banner just displayed,
        // even when every row shares one timestamp (burst of same-millisecond
        // messages). Pre-fix the tie winner was hash iteration order, so the
        // fresh row could be evicted immediately (banner body goes stale).
        RecordingNotifications sent;
        NotificationService service;
        service.setTransport(&sent);
        QString newestUuid;
        for (int i = 0; i < 66; ++i) {
            NotificationCandidate candidate;
            candidate.owner = QStringLiteral("owner-tie");
            newestUuid = QStringLiteral("ffff0000-0000-4000-8000-%1")
                             .arg(65 - i, 12, 10, QLatin1Char('0'));
            candidate.messageUuid = newestUuid;
            candidate.streamUuid = QStringLiteral("stream-tie");
            candidate.topicUuid = QStringLiteral("topic-tie");
            candidate.authorUuid = QStringLiteral("eeee0000-0000-4000-8000-eeeeeeeeeeee");
            candidate.content = QStringLiteral("Tied body %1").arg(i);
            candidate.senderName = QStringLiteral("Tie");
            candidate.topicName = QStringLiteral("Ties");
            candidate.streamMode = QStringLiteral("all_messages");
            candidate.topicMode = QStringLiteral("default");
            candidate.streamKnown = true;
            candidate.topicKnown = true;
            candidate.createdMicros = 480LL * 1000000; // identical timestamps
            service.ingest(candidate);
        }
        const auto tag = sent.lastTag();
        QVERIFY(!tag.isEmpty());
        // The newest message must still be displayable (its body is not the
        // evicted one): the count kept growing and rows stayed at the bound.
        QCOMPARE(service.aggregateCount(tag), 66);
        QCOMPARE(service.aggregateRowCount(tag), 64);
        service.setTransport(nullptr);
    }

    void signOutClosesGenericFallbackBanners()
    {
        // Generic fallback banners (metadata unresolved past the grace window)
        // carry the msg: tag scheme; sign-out must close them too.
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(150, 30);
        openBackgroundConversation(fixture);
        auto arrival = incomingDto(historyMessage(520), notificationAuthor);
        arrival["stream_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999998");
        arrival["topic_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999998");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 21)));
        QTest::qWait(600); // grace (150 ms) + retry tick elapse, generic banner shows
        QVERIFY(!sent.shows.isEmpty());
        const auto tag = sent.lastTag();
        fixture.controller.logout();
        QVERIFY2(sent.closed.contains(tag),
                 "sign-out must tag-close generic fallback banners");
        service->setTransport(nullptr);
    }

    void aggregateRowsPerTagAreBounded()
    {
        // The aggregate count may grow freely, but one bucket must not retain
        // an unbounded number of message bodies on the client.
        RecordingNotifications sent;
        NotificationService service;
        service.setTransport(&sent);
        for (int i = 0; i < 70; ++i) {
            NotificationCandidate candidate;
            candidate.owner = QStringLiteral("owner-rows");
            candidate.messageUuid =
                QStringLiteral("bbbb0000-0000-4000-8000-%1").arg(i, 12, 10, QLatin1Char('0'));
            candidate.streamUuid = QStringLiteral("stream-rows");
            candidate.topicUuid = QStringLiteral("topic-rows");
            candidate.authorUuid = QStringLiteral("cccc0000-0000-4000-8000-cccccccccccc");
            candidate.content = QStringLiteral("Body %1").arg(i);
            candidate.senderName = QStringLiteral("Rower");
            candidate.topicName = QStringLiteral("Rows");
            candidate.streamMode = QStringLiteral("all_messages");
            candidate.topicMode = QStringLiteral("default");
            candidate.streamKnown = true;
            candidate.topicKnown = true;
            candidate.createdMicros = 480LL * 1000000 + i;
            service.ingest(candidate);
        }
        const auto tag = sent.lastTag();
        QVERIFY(!tag.isEmpty());
        QCOMPARE(service.aggregateCount(tag), 70);
        QCOMPARE(service.aggregateRowCount(tag), 64);
        service.setTransport(nullptr);
    }

    void genericFallbackOverflowClosesOldestBanners()
    {
        // Beyond the tracking bound the oldest generic banner is closed to
        // free a slot; no shown banner may end up untracked after sign-out.
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(40, 10);
        openBackgroundConversation(fixture);
        for (int i = 0; i < 300; ++i) {
            auto dto = incomingDto(historyMessage(4000 + i), notificationAuthor);
            dto["stream_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999997");
            dto["topic_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999997");
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", dto, 9000 + i)));
            if (i % 40 == 39) QTest::qWait(300);
        }
        QVERIFY(sent.shows.size() > 256);
        fixture.controller.logout();
        const QSet<QString> closedSet(sent.closed.begin(), sent.closed.end());
        for (const auto &show : sent.shows)
            QVERIFY2(closedSet.contains(show.tag),
                     qPrintable(QStringLiteral("banner %1 was never closed").arg(show.tag)));
        service->setTransport(nullptr);
    }

    // --- issue #86: call awareness (docs/NATIVE_DESKTOP_CALL_AWARENESS.md) ---

    void callStartedHoldsInhibitCookieAndSetsTrayTooltip()
    {
        PowerBusFixture bus("call-screensaver");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        ScreenSaverStub saver;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        // The wiring main.cpp installs: every state change repaints the real
        // tray tooltip through NativeDesktop (issue #86).
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return false; });
        QObject::connect(&awareness, &CallAwareness::callStateChanged,
            &desktop, &NativeDesktop::setCallState);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray);
        QCOMPARE(tray->toolTip(), QString("Workspace"));
        awareness.callStarted("standup", 3);
        // The tooltip repaint is synchronous; the freedesktop cookie arrives
        // with the async Inhibit reply (inhibit is never UI-blocking).
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 1701u, 3000);
        QCOMPARE(saver.inhibits.size(), 1);
        QVERIFY(!saver.inhibits.first().first.isEmpty()); // application id sent
        QCOMPARE(awareness.activeRoom(), QString("standup"));
        // The count from call:start is not rendered (reference parity).
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: standup"));
        awareness.callParticipantsUpdated(5);
        QCOMPARE(saver.inhibits.size(), 1); // the inhibit is not re-acquired
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: standup (5 participants)"));
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, 3000);
        QCOMPARE(saver.uninhibits.first(), 1701u);
        QVERIFY(!awareness.callActive());
        QCOMPARE(tray->toolTip(), QString("Workspace"));
    }

    void callRoomSanitizationFollowsTheReferenceRules()
    {
        QCOMPARE(workspace::CallAwareness::sanitizeRoomName("standup"), QString("standup"));
        QCOMPARE(workspace::CallAwareness::sanitizeRoomName(QString("ca\x01ll\x1fna\x07me\x7f")),
            QString("callname"));
        QCOMPARE(workspace::CallAwareness::sanitizeRoomName(QString(200, 'r')), QString(128, 'r'));
        QCOMPARE(workspace::CallAwareness::sanitizeRoomName(QString()), QString("Call"));
        QCOMPARE(workspace::CallAwareness::sanitizeRoomName(QString("\x01\x02\x1f\x7f")),
            QString("Call"));
        // The reference strips control chars and caps, but never trims.
        QCOMPARE(workspace::CallAwareness::sanitizeRoomName(QString("   ")), QString("   "));
        // A long room with interleaved control chars: strip first, then cap at
        // 128 (reference order: replace().slice()).
        QString raw;
        for (int i = 0; i < 160; ++i) raw += QChar('a' + (i % 26));
        raw.insert(10, QString("\x01\x02\x03"));
        raw.insert(50, QString("\x7f\x1f"));
        const auto clean = workspace::CallAwareness::sanitizeRoomName(raw);
        QCOMPARE(clean.size(), 128);
        QVERIFY(clean == QString(raw).remove(QChar('\x01')).remove(QChar('\x02'))
            .remove(QChar('\x03')).remove(QChar('\x7f')).remove(QChar('\x1f')).left(128));
        // End to end: a control-only room falls back to "Call" in the tooltip,
        // and the service keeps working on a bus with no inhibitor daemon.
        PowerBusFixture bus("call-sanitize");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return false; });
        QObject::connect(&awareness, &CallAwareness::callStateChanged,
            &desktop, &NativeDesktop::setCallState);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray);
        awareness.callStarted(QString("\x07\x07\x7f"), 1);
        QCOMPARE(awareness.activeRoom(), QString("Call"));
        QCOMPARE(awareness.inhibitCookie(), 0u); // no service answered
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: Call"));
        awareness.callEnded(); // releasing a cookie that was never held is a no-op
        QCOMPARE(tray->toolTip(), QString("Workspace"));
    }

    void joinedCallUpdatesTrayAndKeepsOneInhibitorThroughPip()
    {
        PowerBusFixture bus("joined-call-awareness");
        QVERIFY(bus.ok);
        ScreenSaverStub saver;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        Fixture fixture;
        fixture.serverSettings = [](const Request &) { return callMeetSettings(); };
        QVERIFY(fixture.start());
        QTRY_VERIFY(fixture.controller.canStartCall());

        CallAwareness awareness;
        awareness.start(*fixture.controller.callWindow(), bus.watcherBus);
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return false; });
        connect(&awareness, &CallAwareness::callStateChanged, &desktop, &NativeDesktop::setCallState);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray);
        const auto idleTip = tray->toolTip();
        auto *shell = fixture.controller.callWindow();
        auto *bridge = fixture.controller.embedBridge();
        fixture.controller.joinCall("https://meet.example.com/cassi-room");
        QCOMPARE(shell->embedState(), JitsiCallWindowController::Loading);
        shell->apiLoadFailed();
        QCOMPARE(shell->embedState(), JitsiCallWindowController::LoadFailed);
        QVERIFY(!awareness.callActive());
        QCOMPARE(tray->toolTip(), idleTip);
        QCOMPARE(saver.inhibits.size(), 0);

        shell->retry();
        shell->apiLoadSucceeded();
        QCOMPARE(shell->embedState(), JitsiCallWindowController::Ready);
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 1701u, 3000);
        QCOMPARE(saver.inhibits.size(), 1);
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: cassi-room"));
        bridge->reportParticipantCount(3);
        bridge->handleParticipantsJoined();
        QCOMPARE(shell->participantCount(), 3);
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: cassi-room (3 participants)"));
        shell->toggleMinimize();
        QCOMPARE(shell->mode(), JitsiCallWindowController::Minimized);
        bridge->reportParticipantCount(2);
        bridge->handleParticipantsLeft();
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: cassi-room (2 participants)"));
        shell->toggleMinimize();
        QCOMPARE(shell->mode(), JitsiCallWindowController::Expanded);
        QCOMPARE(saver.inhibits.size(), 1);
        QVERIFY(saver.uninhibits.isEmpty());
        shell->readyToClose();
        QVERIFY(!fixture.controller.calls()->hasActiveCall());
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, 3000);
        QCOMPARE(tray->toolTip(), idleTip);
        QVERIFY(!awareness.callActive());

        fixture.controller.joinCall("https://meet.example.com/cassi-next");
        shell->apiLoadSucceeded();
        QTRY_COMPARE_WITH_TIMEOUT(saver.inhibits.size(), 2, 3000);
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: cassi-next"));
        fixture.controller.calls()->closeCall();
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 2, 3000);
        QCOMPARE(tray->toolTip(), idleTip);
    }

    void attachingToAReadyCallReleasesTheInhibitorWhenItsWindowIsDestroyed()
    {
        PowerBusFixture bus("ready-call-awareness");
        QVERIFY(bus.ok);
        ScreenSaverStub saver;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        CallAwareness awareness;
        {
            JitsiCallWindowController shell;
            shell.setParticipantCountProvider([] { return 4; });
            shell.open({{"meetingUrl", "https://meet.jit.si/cassi-ready"}});
            shell.apiLoadSucceeded();
            QCOMPARE(shell.participantCount(), 4);
            QSignalSpy changes(&awareness, &CallAwareness::callStateChanged);
            awareness.start(shell, bus.watcherBus);
            QCOMPARE(awareness.activeRoom(), QString("cassi-ready"));
            QCOMPARE(changes.last().at(1).toInt(), 4);
            QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 1701u, 3000);
        }
        QVERIFY(!awareness.callActive());
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, 3000);
    }

    void displaySleepInhibitIsAcquiredExactlyOncePerCall()
    {
        PowerBusFixture bus("call-once");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        ScreenSaverStub saver;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        awareness.callStarted("first", 2);
        awareness.callStarted("second", 2); // a re-start must not re-inhibit
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 1701u, 3000);
        QCOMPARE(saver.inhibits.size(), 1);
        QCOMPARE(awareness.activeRoom(), QString("second"));
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, 3000);
        QCOMPARE(awareness.inhibitCookie(), 0u);
        awareness.callStarted("third", 1); // the next call acquires afresh
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 1701u, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(saver.inhibits.size(), 2, 3000);
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 2, 3000);
    }

    void powerManagementFallbackAndIdleUpdatesAreIgnored()
    {
        PowerBusFixture bus("call-pmfallback");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        PmInhibitStub pm;
        new PmInhibitAdaptor(&pm);
        QVERIFY(bus.exportObject("/org/freedesktop/PowerManagement/Inhibit", &pm));
        QVERIFY(bus.claimService("org.freedesktop.PowerManagement.Inhibit"));
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        QWindow window;
        NativeDesktop desktop(&window, nullptr, [] { return false; });
        QObject::connect(&awareness, &CallAwareness::callStateChanged,
            &desktop, &NativeDesktop::setCallState);
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray);
        // While idle a call:update changes nothing (reference activeCallRoom guard).
        awareness.callParticipantsUpdated(2);
        QCOMPARE(tray->toolTip(), QString("Workspace"));
        // No ScreenSaver on this bus: the PowerManagement.Inhibit fallback
        // answers and its cookie is the one held and released.
        awareness.callStarted("retro", 1);
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 4242u, 3000);
        QCOMPARE(pm.inhibits.size(), 1);
        awareness.callParticipantsUpdated(3);
        QCOMPARE(tray->toolTip(), QString("Workspace - In call: retro (3 participants)"));
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(pm.uninhibits.size(), 1, 3000);
        QCOMPARE(pm.uninhibits.first(), 4242u);
        QCOMPARE(tray->toolTip(), QString("Workspace"));
    }

    void awarenessDegradesOnABusWithoutInhibitServices()
    {
        PowerBusFixture bus("call-bare");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        QSignalSpy states(&awareness, &CallAwareness::callStateChanged);
        // A connected bus with neither service: inhibition silently stays off
        // (cookie 0) while the call state machine keeps working unbroken.
        awareness.callStarted("nomad", 2);
        QTest::qWait(500); // past the async timeout window of both services
        QCOMPARE(awareness.inhibitCookie(), 0u);
        QCOMPARE(awareness.activeRoom(), QString("nomad"));
        awareness.callParticipantsUpdated(4);
        awareness.callEnded();
        QVERIFY(!awareness.callActive());
        QCOMPARE(awareness.inhibitCookie(), 0u);
        QCOMPARE(states.count(), 3);
    }

    // --- issue #86 fix round: regression tests for the exordy findings ---

    void lateInhibitReplyAfterCallEndDoesNotLeakInhibitor()
    {
        // Finding 1: ending the call while the Inhibit request is pending
        // used to leave the pending guard set, so no later call could ever
        // acquire an inhibitor again.
        PowerBusFixture bus("call-latereply");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        ScreenSaverStub saver;
        saver.inhibitDelayMs = 400;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        awareness.callStarted("quick", 2);
        awareness.callEnded(); // while the Inhibit is still in flight
        QTRY_VERIFY_WITH_TIMEOUT(saver.inhibits.size() >= 1, 3000);
        // The next call acquires its own inhibitor instead of being rejected
        // by the stale pending flag from the ended call.
        awareness.callStarted("next", 1);
        QTRY_COMPARE_WITH_TIMEOUT(saver.inhibits.size(), 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 1701u, 3000);
        // The late reply of the ended call granted an inhibitor nobody asked
        // for any more: it is released the moment it lands, so display sleep
        // is never left stuck.
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, 3000);
        QCOMPARE(saver.uninhibits.first(), 1701u);
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 2, 3000);
    }

    void inhibitedCallStateSurvivesASlowInhibitServiceGrantingLate()
    {
        // Findings 2 and 3: a service that answers Inhibit after the
        // client-side deadline must never leave the inhibitor held without a
        // known cookie. The timeout may discard the grant for the call, but
        // the late reply still reaches the client and is released the moment
        // it lands - the deadline never throws away the way back.
        PowerBusFixture bus("call-slowlate");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        ScreenSaverStub saver;
        saver.inhibitDelayMs = InhibitCallTimeoutMs + 600;
        saver.cookie = 1900;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        awareness.callStarted("patience", 4);
        awareness.callEnded(); // the service is still chewing on the request
        // Past the deadline: nothing is treated as pending any more.
        QTest::qWait(InhibitCallTimeoutMs + 200);
        QCOMPARE(awareness.inhibitCookie(), 0u);
        QTRY_VERIFY_WITH_TIMEOUT(saver.inhibits.size() >= 1, InhibitCallTimeoutMs + 2500);
        // Past the late reply: the granted cookie was released even though
        // the call is over and no cookie was ever held by the client.
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, InhibitCallTimeoutMs + 2500);
        QCOMPARE(saver.uninhibits.first(), 1900u);
        // And the state machine is fully reusable for the next call.
        saver.inhibitDelayMs = 0;
        saver.cookie = 1901;
        awareness.callStarted("after", 2);
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 1901u, 3000);
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 2, 3000);
        QCOMPARE(saver.uninhibits.last(), 1901u);
    }

    void lateScreenSaverGrantIsRetainedWhenTheFallbackFailed()
    {
        // exordy finding (review 4627245017): the client deadline marks a
        // slow ScreenSaver reply late after InhibitCallTimeoutMs, the
        // PowerManagement fallback finds no service on this bus, and the
        // late ScreenSaver grant - the only inhibitor this call will ever
        // get - used to be released the moment it landed, leaving the
        // active call uninhibited. A late grant for a still-live call with
        // no other inhibitor held must be retained and released only when
        // the call ends.
        PowerBusFixture bus("call-late-retained");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        ScreenSaverStub saver;
        saver.inhibitDelayMs = InhibitCallTimeoutMs + 600;
        saver.cookie = 1933;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        // No PowerManagement.Inhibit on this bus: the fallback fired at the
        // deadline is answered with an error at once and grants nothing.
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        awareness.callStarted("patience", 2);
        // Past the deadline: the fallback was asked and failed; nothing is
        // held while the ScreenSaver service is still chewing on the grant.
        QTest::qWait(InhibitCallTimeoutMs + 200);
        QVERIFY(awareness.callActive());
        QCOMPARE(awareness.inhibitCookie(), 0u);
        // Past the late reply (it lands ~InhibitCallTimeoutMs + 600): the
        // call adopts the late grant instead of throwing it back - no
        // UnInhibit may be sent while the call is active and nothing else
        // inhibits. Before the fix this released cookie 1933 immediately
        // and the active call stayed uninhibited.
        QTest::qWait(900);
        QCOMPARE(saver.inhibits.size(), 1);
        QCOMPARE(saver.uninhibits.size(), 0);
        QCOMPARE(awareness.inhibitCookie(), 1933u);
        // callEnded releases exactly that cookie, once.
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, 3000);
        QCOMPARE(saver.uninhibits.first(), 1933u);
        QVERIFY(!awareness.callActive());
        QCOMPARE(awareness.inhibitCookie(), 0u);
    }

    void lateScreenSaverGrantIsReleasedWhenFallbackAlreadyHoldsInhibitor()
    {
        // The other half of the same fix: retention applies only while no
        // other inhibitor is held. Here the PowerManagement fallback
        // answers promptly at the deadline, so the late ScreenSaver grant
        // is a duplicate and must still be released the moment it lands;
        // the call ends releasing exactly the held fallback cookie.
        PowerBusFixture bus("call-late-duplicate");
        if (!bus.ok) QSKIP("a private session bus is unavailable");
        ScreenSaverStub saver;
        saver.inhibitDelayMs = InhibitCallTimeoutMs + 600;
        saver.cookie = 1944;
        new ScreenSaverAdaptor(&saver);
        QVERIFY(bus.exportObject("/ScreenSaver", &saver));
        QVERIFY(bus.claimService("org.freedesktop.ScreenSaver"));
        PmInhibitStub pm;
        new PmInhibitAdaptor(&pm);
        QVERIFY(bus.exportObject("/org/freedesktop/PowerManagement/Inhibit", &pm));
        QVERIFY(bus.claimService("org.freedesktop.PowerManagement.Inhibit"));
        CallAwareness awareness;
        awareness.start(bus.watcherBus);
        awareness.callStarted("fallback", 1);
        // The deadline falls through to PowerManagement, which grants at once.
        QTRY_COMPARE_WITH_TIMEOUT(awareness.inhibitCookie(), 4242u, InhibitCallTimeoutMs + 1500);
        // Past the late ScreenSaver reply: it lands onto a call that is
        // already inhibited, so the extra grant is returned right away and
        // the held fallback cookie stays put.
        QTest::qWait(InhibitCallTimeoutMs + 900);
        QCOMPARE(saver.inhibits.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(saver.uninhibits.size(), 1, 3000);
        QCOMPARE(saver.uninhibits.first(), 1944u);
        QCOMPARE(awareness.inhibitCookie(), 4242u);
        // callEnded releases exactly the held fallback cookie, once.
        awareness.callEnded();
        QTRY_COMPARE_WITH_TIMEOUT(pm.uninhibits.size(), 1, 3000);
        QCOMPARE(pm.uninhibits.first(), 4242u);
        QCOMPARE(saver.uninhibits.size(), 1);
    }

    void futureCallSurfaceInvokesTheQmlInvokables()
    {
        // The dormant service is reachable exactly through these three
        // Q_INVOKABLEs (the seam the future call UI and any QML surface use).
        const auto &meta = workspace::CallAwareness::staticMetaObject;
        QVERIFY(meta.indexOfMethod("callStarted(QString,int)") >= 0);
        QVERIFY(meta.indexOfMethod("callParticipantsUpdated(int)") >= 0);
        QVERIFY(meta.indexOfMethod("callEnded()") >= 0);
    }
};

QTEST_MAIN(ControllerNotificationsTest)
#include "native-controller-notifications-test.moc"
