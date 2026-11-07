#include "mock-server.h"
#include "native-app.h"
#include "native-controller.h"
#include "native-chat-crypto.h"
#include "native-markdown.h"
#include "native-cache-service.h"
#include "native-preferences.h"
#include "file-client.h"

#include <QCryptographicHash>
#include <QApplication>
#include <QAccessible>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QOperatingSystemVersion>
#include <QPlatformSurfaceEvent>
#include <QLocale>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlIncubator>
#include <QJSValue>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QPointer>
#include <QSGRendererInterface>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QWheelEvent>
#include <QtTest/qtestwheel.h>

using namespace workspace;
using namespace workspace::test;

static QQuickItem *item(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name)
        return root;
    for (auto *child : root->childItems())
        if (auto *found = item(child, name))
            return found;
    return nullptr;
}

static void click(QQuickWindow *window, QQuickItem *target, QPointF *dispatchedAt = nullptr)
{
    QVERIFY(target);
    QTest::qWait(50);
    QVERIFY(target->isVisible());
    QVERIFY(target->width() > 0 && target->height() > 0);
    const auto point = target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
    if (dispatchedAt) *dispatchedAt = point;
    QTest::mouseClick(window, Qt::LeftButton, {}, point.toPoint());
}

static int itemCountWithPrefix(QQuickItem *root, const QString &prefix)
{
    int count = root->objectName().startsWith(prefix);
    for (auto *child : root->childItems()) count += itemCountWithPrefix(child, prefix);
    return count;
}

static bool hasVisibleText(QQuickItem *root, const QString &text)
{
    if (root->isVisible() && root->property("text").toString() == text) return true;
    for (auto *child : root->childItems())
        if (hasVisibleText(child, text)) return true;
    return false;
}

static QQuickItem *fileEntry(QQuickItem *root, const QUrl &url)
{
    if (root->isVisible() && root->property("fileUrl").toUrl() == url) return root;
    for (auto *child : root->childItems())
        if (auto *found = fileEntry(child, url)) return found;
    return nullptr;
}

static QQuickItem *fileEntry(const QUrl &url)
{
    for (auto *candidate : QGuiApplication::allWindows())
        if (auto *window = qobject_cast<QQuickWindow *>(candidate))
            if (auto *found = fileEntry(window->contentItem(), url)) return found;
    return nullptr;
}

static QQuickItem *visibleItem(QQuickItem *root, const QString &name)
{
    if (root->isVisible() && root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = visibleItem(child, name)) return found;
    return nullptr;
}

static QQuickItem *visibleItem(const QString &name)
{
    for (auto *candidate : QGuiApplication::allWindows())
        if (auto *window = qobject_cast<QQuickWindow *>(candidate))
            if (auto *found = visibleItem(window->contentItem(), name)) return found;
    return nullptr;
}

static void openFullReactionPicker(QQuickWindow *window, QQuickItem *bubble)
{
    QVERIFY(bubble);
    QTest::mouseClick(window, Qt::RightButton, {},
        bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
    QTRY_VERIFY(visibleItem("moreReactionsButton"));
    click(window, visibleItem("moreReactionsButton"));
}

static QQuickItem *dialogButton(QQuickItem *root, const QString &caption)
{
    if (root->isVisible() && root->metaObject()->indexOfSignal("clicked()") >= 0
        && root->property("text").toString().remove('&') == caption) return root;
    for (auto *child : root->childItems())
        if (auto *found = dialogButton(child, caption)) return found;
    return nullptr;
}

static QQuickItem *dialogButton(const QString &caption)
{
    for (auto *candidate : QGuiApplication::allWindows())
        if (auto *window = qobject_cast<QQuickWindow *>(candidate))
            if (auto *found = dialogButton(window->contentItem(), caption)) return found;
    return nullptr;
}

static void awaitTopicMenuLayout(QQuickWindow *window,QQuickItem *action,const QString &expected)
{
    auto *menu=window->findChild<QObject *>("streamContextMenu"); QVERIFY(menu);
    auto *list=menu->property("contentItem").value<QQuickItem *>(); QVERIFY(list);
    const auto ready=[&] {
        if(window->property("streamMenuTarget").toString()!=expected || !menu->property("visible").toBool() || !menu->property("opened").toBool()
            || !action->isVisible() || !action->isEnabled() || action->window()!=window || action->height()!=action->implicitHeight()) return false;
        const auto content=list->property("contentHeight").toReal();
        if(content!=action->height() || menu->property("height").toReal()!=content+menu->property("topPadding").toReal()+menu->property("bottomPadding").toReal()) return false;
        auto *popup=list->parentItem();
        return popup && popup->window()==window && QRectF(popup->mapToScene(QPointF{}),popup->size()).contains(QRectF(action->mapToScene(QPointF{}),action->size()));
    };
    // Menu.opened precedes ListView's first content-height layout. A visible
    // 36px row can temporarily extend outside its padding-only 10px popup.
    QTRY_VERIFY2(ready(),"The real menu must contain the complete source row before mouse input");
}

static void openStreamTopicForm(QQuickWindow *window, const QString &id)
{
    QPointer<QQuickItem> action;
    QPointF dispatchedAt;
    QVariantMap dispatchState;
    std::unique_ptr<QSignalSpy> pressed, released, canceled, clicked, triggered;
    const auto failure = qScopeGuard([&] {
        if (visibleItem("topicNameInput")) return;
        auto *menu = window->findChild<QObject *>("streamContextMenu");
        qWarning() << "Topic menu failure" << "expectedTarget" << id
            << "target" << window->property("streamMenuTarget")
            << "available" << window->property("streamMenuAvailable")
            << "menuVisible" << (menu ? menu->property("visible") : QVariant{})
            << "menuOpened" << (menu ? menu->property("opened") : QVariant{})
            << "enabled" << (action && action->isEnabled())
            << "coordinates" << dispatchedAt << "targetWindow" << (action ? action->window() : nullptr)
            << "givenWindow" << window << "formTarget" << window->property("topicFormTarget")
            << "owner" << window->property("streamMenuOwner") << "route" << window->property("streamMenuRoute") << "acl" << window->property("streamMenuContext")
            << "dispatchState" << dispatchState << "signals"
            << (pressed ? pressed->size() : 0) << (released ? released->size() : 0)
            << (canceled ? canceled->size() : 0) << (clicked ? clicked->size() : 0) << (triggered ? triggered->size() : 0);
        const auto directory = qEnvironmentVariable("CASSI_NATIVE_FAILURE_SCREENSHOT_DIRECTORY", QDir::tempPath());
        const auto path = QDir(directory).filePath(QString("cassi-native-topic-failure-%1.png").arg(QCoreApplication::applicationPid()));
        if (window->grabWindow().save(path)) qWarning() << "Failure screenshot" << path;
    });
    auto *stream = item(window->contentItem(), "stream-" + id);
    QVERIFY(stream);
    QTest::mouseClick(window, Qt::RightButton, {},
        stream->mapToScene(QPointF(20, stream->height() / 2)).toPoint());
    QTRY_VERIFY(visibleItem("streamNewTopicAction"));
    action = visibleItem("streamNewTopicAction");
    dispatchState = {{"enabled", action->isEnabled()}, {"target", window->property("streamMenuTarget")},
        {"route", window->property("streamMenuRoute")}, {"acl", window->property("streamMenuContext")}};
    pressed = std::make_unique<QSignalSpy>(action, SIGNAL(pressed()));
    released = std::make_unique<QSignalSpy>(action, SIGNAL(released()));
    canceled = std::make_unique<QSignalSpy>(action, SIGNAL(canceled()));
    clicked = std::make_unique<QSignalSpy>(action, SIGNAL(clicked()));
    triggered = std::make_unique<QSignalSpy>(action, SIGNAL(triggered()));
    QVERIFY(pressed->isValid() && released->isValid() && canceled->isValid() && clicked->isValid() && triggered->isValid());
    awaitTopicMenuLayout(window,action,id);
    click(window, action, &dispatchedAt);
    QTRY_VERIFY(visibleItem("topicNameInput"));
}

class TopicCompletionProbe final : public QObject {
    Q_OBJECT
signals:
    void completed(const QString &stream, const QString &uuid);
};

class WindowSecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    bool holdRead = false;
    Error readError = Error::None;
    std::function<void()> pendingRead;
    void write(const QString &key, const QByteArray &secret, Completion done) override {
        values.insert(key, secret);
        QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void read(const QString &key, Loaded done) override {
        auto operation = [this, key, done] {
            done(readError == Error::None ? values.value(key) : QByteArray{}, readError);
        };
        if (holdRead) pendingRead = std::move(operation);
        else QTimer::singleShot(0, this, std::move(operation));
    }
    void remove(const QString &key, Completion done) override {
        values.remove(key);
        QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void finishRead() {
        QVERIFY(pendingRead);
        auto operation = std::move(pendingRead);
        pendingRead = {};
        operation();
    }
};

static const QString channelPeer = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
static const QString channelDefault = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";

static QJsonObject windowChannel(const QString &id, const QString &name)
{
    return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"owner", userId},
        {"name", name}, {"description", ""}, {"role", "owner"}, {"notification_mode", "all_messages"},
        {"source_name", "native"}, {"provider_uuid", QJsonValue::Null}, {"provider_identity", QJsonValue::Null},
        {"invite_only", false}, {"announce", false}, {"private", false}, {"is_archived", false},
        {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null}, {"direct_user_uuid", QJsonValue::Null},
        {"default_topic_uuid", channelDefault}, {"unread_count", 0}, {"active_unread_count", 0},
        {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

static QJsonObject windowChannelTopic(const QString &id, const QString &stream, const QString &name)
{
    return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"stream_uuid", stream},
        {"name", name}, {"is_default", true}, {"is_done", false}, {"notification_mode", "default"},
        {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0},
        {"last_message_uuid", QJsonValue::Null}, {"created_at", "2026-01-02T10:00:00Z"},
        {"updated_at", "2026-01-02T10:00:00Z"}};
}

class WindowChannelFixture {
public:
    MockServer server;
    QJsonObject created, defaultTopic;
    QJsonArray bindings;
    QJsonArray users{QJsonObject{{"uuid", userId}, {"username", "cassi"}},
        QJsonObject{{"uuid", channelPeer}, {"username", "alice"}, {"first_name", "Alice"},
                    {"email", "alice@example.invalid"}}};
    int posts = 0, canonicalGets = 0, topicGets = 0, catalogGets = 0, memberPosts = 0,
        bindingGets = 0, usersGets = 0, historyGets = 0;
    int postStatus = 201, memberStatus = 200, canonicalStatus = 200;
    int postDelay = 150, canonicalDelay = 80, topicDelay = 0, catalogDelay = 0;
    bool applyMembers = true, includeCreated = true, includeDefault = true;
    bool sourceScene = false;
    QJsonObject streamBody, memberBody;

    WindowChannelFixture()
    {
        server.handler = [this](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/streams/") && request.method == "POST") {
                ++posts;
                streamBody = request.body;
                created = windowChannel(request.body.value("uuid").toString(), request.body.value("name").toString());
                for (const auto *field : {"description", "invite_only", "announce"}) created[field] = request.body.value(field);
                defaultTopic = windowChannelTopic(channelDefault, created.value("uuid").toString(), "General Topic");
                return {created, postStatus, postDelay};
            }
            if (!created.isEmpty() && path.endsWith("/streams/" + created.value("uuid").toString())) {
                ++canonicalGets; return {created, canonicalStatus, canonicalDelay};
            }
            if (path.endsWith("/streams/") && request.method == "GET") {
                ++catalogGets;
                auto existing = windowChannel(streamId, "Product team");
                if (sourceScene) {
                    existing["description"] = "Native preview";
                    existing["unread_count"] = 3;
                    existing["default_topic_uuid"] = topicId;
                }
                QJsonArray rows{existing};
                if (!created.isEmpty() && includeCreated) rows.append(created);
                return {rows, 200, created.isEmpty() ? 0 : catalogDelay};
            }
            if (path.endsWith("/users/")) { ++usersGets; return {users}; }
            if (path.endsWith("/stream_topics/")) {
                const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                if (!created.isEmpty() && stream == created.value("uuid").toString()) {
                    ++topicGets; return {includeDefault ? QJsonArray{defaultTopic} : QJsonArray{}, 200, topicDelay};
                }
                QJsonArray topics{windowChannelTopic(topicId, streamId, "General Topic")};
                if (sourceScene) {
                    auto planning = windowChannelTopic(secondTopicId, streamId, "Planning");
                    planning["is_default"] = false;
                    auto closed = windowChannelTopic("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", streamId, "Releases");
                    closed["is_default"] = false; closed["is_done"] = true; closed["unread_count"] = 3;
                    topics.append(planning); topics.append(closed);
                }
                return {topics};
            }
            if (path.endsWith("/actions/add_users/invoke")) {
                ++memberPosts; memberBody = request.body;
                QJsonArray result; int number = 1;
                for (const auto &value : request.body.value("member").toArray())
                    result.append(QJsonObject{{"uuid", QString("82000000-0000-4000-8000-%1").arg(number++, 12, 10, QLatin1Char('0'))},
                        {"project_id", projectId}, {"stream_uuid", created.value("uuid")}, {"user_uuid", value},
                        {"who_uuid", userId}, {"role", "member"}, {"notification_mode", "all_messages"},
                        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}});
                if (applyMembers) bindings = result;
                return {result, memberStatus, 120};
            }
            if (path.endsWith("/stream_bindings/")) { ++bindingGets; return {bindings}; }
            if (path.endsWith("/messages/")) {
                ++historyGets;
                QJsonArray rows;
                if (sourceScene && QUrlQuery(request.target).queryItemValue("stream_uuid") == streamId) {
                    const QStringList contents{"General discussion stays in this channel view.",
                        "A second message from the same author and topic.", "Planning now starts a new topic run.",
                        "Own messages carry the topic label once per group.", "A resolved topic remains visible in the aggregate.",
                        "Returning to Planning adds another divider.", "A message with missing topic metadata has no invented label."};
                    const QStringList topics{topicId, topicId, secondTopicId, secondTopicId,
                        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", secondTopicId, "cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
                    for (int index = 0; index < contents.size(); ++index) {
                        auto row = message(contents[index], QString("81000000-0000-4000-8000-%1").arg(index + 1, 12, 10, QLatin1Char('0')), topics[index]);
                        const bool own = index == 3 || index == 4;
                        row["author_uuid"] = own ? userId : channelPeer; row["is_own"] = own;
                        row["created_at"] = QDateTime::fromString("2026-01-02T10:00:00.000Z", Qt::ISODateWithMs).addSecs(index * 60).toString(Qt::ISODateWithMs);
                        rows.append(row);
                    }
                }
                return {rows};
            }
            auto response = sessionResponse(request);
            if (sourceScene && path.endsWith("/actions/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", QJsonObject{{"uuid", userId},
                    {"username", "Cassi"}, {"email", "cassi@example.invalid"}}}};
            return response;
        };
    }
};

static QString windowDirectId(const QString &peer = channelPeer)
{
    QStringList pair{peer.toLower(), userId.toLower()}; pair.sort();
    return QUuid::createUuidV5(QUuid("26f42131-4503-5f59-8aa6-32b20f551751"),
        (projectId.toLower() + ":" + pair.join(':')).toUtf8()).toString(QUuid::WithoutBraces);
}

class WindowDirectFixture : public WindowChannelFixture {
public:
    bool applyDirectPost = true;
    int lookupStatus = 0;
    QVector<QJsonObject> directBodies;
    WindowDirectFixture()
    {
        const auto fallback = server.handler;
        server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/streams/") && request.method == "POST" && request.body.contains("direct_user_uuid")) {
                ++posts; directBodies.append(request.body);
                if (applyDirectPost) seed(request.body.value("direct_user_uuid").toString());
                return {created, postStatus, postDelay};
            }
            if (request.method == "GET" && (path.endsWith("/streams/" + windowDirectId())
                || path.endsWith("/streams/" + windowDirectId(returnedId)))) {
                ++canonicalGets;
                return {created, lookupStatus ? lookupStatus : created.isEmpty() ? 404 : 200, canonicalDelay};
            }
            return fallback(request);
        };
    }
    void seed(const QString &peer = channelPeer)
    {
        created = windowChannel(windowDirectId(peer), "Direct");
        created["description"] = "Private workspace";
        created["private"] = true;
        created["direct_user_uuid"] = peer;
        defaultTopic = windowChannelTopic(channelDefault, windowDirectId(peer), "General Topic");
    }
};

static const QString archiveWindowId="97000000-0000-4000-8000-000000000001";
static QString archiveRowId(int number) { return QString("97000000-0000-4000-8000-%1").arg(number,12,10,QLatin1Char('0')); }
class WindowArchiveFixture : public WindowChannelFixture {
public:
    QJsonArray archived;
    int restorePosts=0,restoreGets=0,archiveCatalogStatus=200,archiveCatalogDelay=0;
    int restoreStatus=200,restoreGetStatus=200,restoreDelay=0;
    bool applyRestore=true;
    QMap<QString,int> rowStatuses,rowDelays;
    WindowArchiveFixture() {
        sourceScene=true;
        const QStringList names{"Stored Alice","Archived team","Design channel with a deliberately long synthetic name that needs to elide inside an archived row","Encrypted archive","Provider archive"};
        for(int n=0;n<names.size();++n) {
            auto row=windowChannel(archiveRowId(n+1),names[n]); row["is_archived"]=true;
            if(n==0) { row["private"]=true; row["direct_user_uuid"]=channelPeer; }
            if(n==3) { row["encryption"]=true; row["current_encryption_key_uuid"]=returnedId; }
            if(n==4) { row["source_name"]="zulip"; row["provider_uuid"]=returnedId; row["provider_identity"]="synthetic-provider"; }
            archived.append(row);
        }
        const auto fallback=server.handler;
        server.handler=[this,fallback](const Request &request)->Response {
            const auto path=request.target.path();
            if(path.endsWith("/streams/") && request.method=="GET") {
                ++catalogGets; QJsonArray result{windowChannel(streamId,"Product team")};
                for(const auto &row:archived) result.append(row);
                return {result,archiveCatalogStatus,archiveCatalogDelay};
            }
            for(int n=0;n<archived.size();++n) {
                auto row=archived[n].toObject(); const auto id=row["uuid"].toString();
                if(path.endsWith("/streams/"+id+"/actions/unarchive/invoke")) {
                    ++restorePosts;
                    if(applyRestore) { row["is_archived"]=false;row["updated_at"]="2026-01-02T10:01:00Z";archived[n]=row; }
                    return {row,rowStatuses.value(id,restoreStatus),rowDelays.value(id,restoreDelay)};
                }
                if(path.endsWith("/streams/"+id)) { ++restoreGets; return {row,restoreGetStatus}; }
            }
            const auto selected=QUrlQuery(request.target).queryItemValue("stream_uuid");
            if(selected.startsWith("97000000-") && path.endsWith("/stream_topics/")) {
                ++topicGets; return {QJsonArray{windowChannelTopic(topicId,selected,"Archived topic")}};
            }
            if(selected.startsWith("97000000-") && path.endsWith("/messages/")) {
                ++historyGets;auto row=message("Saved synthetic archive history");row["stream_uuid"]=selected;return {QJsonArray{row}};
            }
            return fallback(request);
        };
    }
};

static void openArchive(QQuickWindow *window) {
    click(window,item(window->contentItem(),"createChatButton"));
    QTRY_VERIFY(visibleItem("createChatTab-3"));
    click(window,visibleItem("createChatTab-3"));
    QTRY_VERIFY(visibleItem("archiveChatSearch"));
}

static void captureArchiveWindow(QQuickWindow *window,const QString &scenario,const QString &state) {
    const auto dir=qEnvironmentVariable("CASSI_NATIVE_ARCHIVE_SCREENSHOT_DIRECTORY");if(dir.isEmpty())return;
    QTest::mouseMove(window,QPoint(15,window->height()-15));QSignalSpy frame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!frame.isEmpty());
    const auto base=QDir(dir).filePath("native-archive-v19-"+scenario+"-"+state);QVERIFY(window->grabWindow().save(base+".png"));
    QJsonObject data{{"scenario",scenario},{"state",state},{"viewport",QJsonObject{{"width",window->width()},{"height",window->height()}}},{"dpr",window->devicePixelRatio()},{"synthetic",true},{"serviceAcceptance",false},{"graphicsApi",int(window->rendererInterface()->graphicsApi())},{"applicationClass",QCoreApplication::instance()->metaObject()->className()}};
    for(const auto &name:QStringList{"createChatSurface","archiveChatSearch","archiveChatRoster","archiveRestoreAlert","archiveCatalogError"})if(auto *found=visibleItem(name)){const auto at=found->mapToScene(QPointF{});data[name]=QJsonObject{{"x",at.x()},{"y",at.y()},{"width",found->width()},{"height",found->height()}};}
    QFile output(base+".json");QVERIFY(output.open(QIODevice::WriteOnly));output.write(QJsonDocument(data).toJson());
}

class WindowTopicFixture {
public:
    MockServer server;
    QJsonArray streams;
    QJsonObject created, posted;
    int posts = 0, confirmations = 0, catalogs = 0;
    int postStatus = 201, canonicalStatus = 200;
    int postDelay = 100;
    WindowTopicFixture() {
        streams.append(windowChannel(streamId, "Product team"));
        auto direct = windowChannel("88000000-0000-4000-8000-000000000001", "Personal chat");
        direct["private"] = true; direct["direct_user_uuid"] = channelPeer; streams.append(direct);
        auto encrypted = windowChannel("88000000-0000-4000-8000-000000000002", "Encrypted channel");
        encrypted["encryption"] = true; encrypted["current_encryption_key_uuid"] = "88000000-0000-4000-8000-000000000099"; streams.append(encrypted);
        auto provider = windowChannel("88000000-0000-4000-8000-000000000003", "Provider channel");
        provider["source_name"] = "zulip"; provider["provider_uuid"] = "88000000-0000-4000-8000-000000000098"; streams.append(provider);
        auto archived = windowChannel("88000000-0000-4000-8000-000000000004", "Archived excluded");
        archived["is_archived"] = true; streams.append(archived);
        server.handler = [this](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return {streams};
            if (path.endsWith("/users/") || path.endsWith("/messages/")) return {QJsonArray{}};
            if (path.endsWith("/stream_topics/") && request.method == "POST") {
                ++posts; posted = request.body;
                created = windowChannelTopic(posted["uuid"].toString(), posted["stream_uuid"].toString(), posted["name"].toString());
                created["is_default"] = false;
                return {created, postStatus, postDelay};
            }
            if (!created.isEmpty() && path.endsWith("/stream_topics/" + created["uuid"].toString())) {
                ++confirmations; return {canonicalStatus == 200 ? created : QJsonObject{}, canonicalStatus};
            }
            if (path.endsWith("/stream_topics/")) {
                QJsonArray rows;
                if (!created.isEmpty() && QUrlQuery(request.target).queryItemValue("stream_uuid") == created["stream_uuid"].toString()) { ++catalogs; rows.append(created); }
                return {rows};
            }
            return sessionResponse(request);
        };
    }
};

static void openGlobalTopic(QQuickWindow *window)
{
    click(window, item(window->contentItem(), "createChatButton"));
    QTRY_VERIFY(visibleItem("createChatTab-2"));
    click(window, visibleItem("createChatTab-2"));
    QTRY_VERIFY(visibleItem("globalTopicStreamSelector"));
}

static void openChannelNewChat(QQuickWindow *window)
{
    click(window, item(window->contentItem(), "createChatButton"));
    QTRY_VERIFY(visibleItem("createChatTab-1"));
    click(window, visibleItem("createChatTab-1"));
    QTRY_VERIFY(visibleItem("channelNameInput"));
}

static const QString linkRequestedId = "98000000-0000-4000-8000-000000000001";
static const QString linkCanonicalId = "98000000-0000-4000-8000-000000000002";
static const QString linkDestinationId = "98000000-0000-4000-8000-000000000003";
static const QString linkStaleHintId = "98000000-0000-4000-8000-000000000004";

class WindowTopicLinkFixture : public WindowChannelFixture {
public:
    QString scenario, href;
    int lookupGets = 0, detailGets = 0, routeTopicGets = 0;
    int routeHistoryGets = 0, aggregateGets = 0, mutations = 0;
    bool record = false;
    QStringList unexpected;
    QVector<QUrl> historyTargets;
    explicit WindowTopicLinkFixture(const QString &value) : scenario(value)
    {
        href = "urn:topic:" + linkStaleHintId + ":" + (scenario == "known" ? secondTopicId : linkRequestedId);
        const auto fallback = server.handler;
        server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (record && request.method != "GET") {
                ++mutations;
                unexpected.append(QString::fromUtf8(request.method) + " " + request.target.toString());
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/" + linkRequestedId && request.method == "GET") {
                ++lookupGets;
                const bool cross = scenario == "cross-canonical";
                return {windowChannelTopic(cross ? linkCanonicalId : linkRequestedId,
                    cross ? linkDestinationId : streamId, "Linked destination"),
                    scenario == "lookup-error" ? 503 : 200, scenario == "cancel-pending" ? 30000 : 0};
            }
            if (path == "/api/workspace/v1/messenger/streams/" + linkDestinationId && request.method == "GET") {
                ++detailGets;
                return {windowChannel(linkDestinationId, "Destination channel")};
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/" && request.method == "GET") {
                if (record) ++routeTopicGets;
                const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                if (stream == linkDestinationId)
                    return {QJsonArray{windowChannelTopic(linkCanonicalId, stream, "Linked destination")}};
                QJsonArray topics{windowChannelTopic(topicId, streamId, "General Topic"),
                    windowChannelTopic(secondTopicId, streamId, "Planning")};
                if (record && scenario == "unknown-same")
                    topics.append(windowChannelTopic(linkRequestedId, streamId, "Linked destination"));
                return {topics};
            }
            if (path == "/api/workspace/v1/messenger/messages/" && request.method == "GET") {
                const auto query = QUrlQuery(request.target);
                const auto stream = query.queryItemValue("stream_uuid");
                const auto topic = query.queryItemValue("topic_uuid");
                if (record) {
                    historyTargets.append(request.target);
                    if (topic.isEmpty()) ++aggregateGets; else ++routeHistoryGets;
                }
                auto row = message(topic == topicId || topic.isEmpty()
                    ? "[Open linked topic](" + href + ")" : "The canonical topic is open",
                    topic == topicId || topic.isEmpty() ? messageId : returnedId, topic.isEmpty() ? topicId : topic);
                row["stream_uuid"] = stream;
                return {QJsonArray{row}};
            }
            if (record) {
                // A link must not reload global catalogs or the user directory.
                unexpected.append(QString::fromUtf8(request.method) + " " + request.target.toString());
            }
            return fallback(request);
        };
    }
};

// Query the displayed text's actual anchor geometry, then send mouse input.
// The public linkActivated signal is the witness that the real link received it.
static QPointF topicLinkPoint(QQuickItem *text, const QString &expected)
{
    for (qreal y = 1; y < text->height(); y += 2) {
        for (qreal x = 1; x < text->width(); x += 2) {
            QString href;
            if (!QMetaObject::invokeMethod(text, "linkAt", Q_RETURN_ARG(QString, href), Q_ARG(qreal, x), Q_ARG(qreal, y)))
                return QPointF(-1, -1);
            if (href == expected) return QPointF(x, y);
        }
    }
    return QPointF(-1, -1);
}

// Observe only public focus/window/surface events. This does not claim a
// Wayland protocol roundtrip or attribute an earlier warning to this window.
class WindowFocusLifecycleProbe final : public QObject {
    QPointer<QWindow> m_window;
    QJsonArray m_events;
    QString m_nativeId;
    int m_order=0, m_focusLost=0, m_surfaceDestroyed=0, m_destroyed=0;
    bool m_armed=false, m_surfaceLive=false;
    void record(const char *event) {
        if(!m_armed) return;
        const auto order=++m_order;
        const bool active=m_window && m_window->isActive();
        const bool focused=m_window && QGuiApplication::focusWindow()==m_window.data();
        if(m_window && m_surfaceLive && !active && !focused && !m_focusLost) m_focusLost=order;
        if(m_events.size()<64) m_events.append(QJsonObject{{"order",order},{"event",QString::fromLatin1(event)},
            {"active",active},{"focusWindowIsTarget",focused},{"surfaceLive",m_surfaceLive}});
    }
protected:
    bool eventFilter(QObject *object,QEvent *event) override {
        if(object!=m_window.data()) return false;
        if(event->type()==QEvent::FocusOut) record("FocusOut");
        else if(event->type()==QEvent::Hide) record("Hide");
        else if(event->type()==QEvent::Close) record("Close");
        else if(event->type()==QEvent::PlatformSurface) {
            const auto kind=static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType();
            if(kind==QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                record("SurfaceAboutToBeDestroyed");
                if(!m_surfaceDestroyed) m_surfaceDestroyed=m_order;
                m_surfaceLive=false;
            } else { m_surfaceLive=true; record("SurfaceCreated"); }
        }
        return false;
    }
public:
    void watch(QWindow *window) {
        m_window=window; window->installEventFilter(this);
        connect(window,&QWindow::activeChanged,this,[this] {record("activeChanged");});
        connect(qGuiApp,&QGuiApplication::focusWindowChanged,this,[this](QWindow *) {record("focusWindowChanged");});
        connect(window,&QObject::destroyed,this,[this] {++m_destroyed;record("QObjectDestroyed");});
    }
    void arm(quintptr nativeId) {m_nativeId=QString::number(nativeId,16);m_surfaceLive=m_window && m_window->isExposed();m_armed=true;record("armed");}
    bool focusLostBeforeSurface() const {return m_focusLost>0 && m_surfaceDestroyed>m_focusLost;}
    int destroyedCount() const {return m_destroyed;}
    int surfaceOrder() const {return m_surfaceDestroyed;}
    QJsonObject facts() const {return {{"focusLossOrder",m_focusLost},{"surfaceDestroyOrder",m_surfaceDestroyed},
        {"QObjectDestroyedCount",m_destroyed},{"nativeIdWhileExposed",m_nativeId},{"events",m_events}};}
};

static bool closeObservedTestWindow(QQuickWindow *window)
{
    const QPointer<QQuickWindow> guard(window);
    if(!guard) return true;
    guard->hide();
    // Public focus acknowledgement, not a sleep or protocol-roundtrip claim.
    const bool inactive=QTest::qWaitFor([guard] {
        return guard && !guard->isActive() && QGuiApplication::focusWindow()!=guard.data();
    },2000);
    if(!inactive) return false;
    QGuiApplication::sync();
    const bool closed=guard && guard->close();
    QGuiApplication::sync();
    return closed;
}

// Natural visible-frame/creation acknowledgement before the existing focus
// teardown. No forceCompletion, incubateFor, forced layout or async disabling.
static bool closeIncubatedTestWindow(QQmlEngine &engine,QQuickWindow *window)
{
    const QPointer<QQuickWindow> guard(window);
    if(!guard)return true;
    auto *incubation=engine.incubationController();
    const bool observedController=incubation&&incubation==guard->incubationController();
    bool settled=false;
    if(observedController&&guard->isVisible()&&guard->isExposed()) {
        QSignalSpy frame(guard.data(),&QQuickWindow::frameSwapped);
        if(frame.isValid()) {
            guard->update();
            settled=QTest::qWaitFor([&] {
                return guard&&guard->isVisible()&&!frame.isEmpty()
                    &&engine.incubationController()==incubation&&incubation->incubatingObjectCount()==0;
            },2000);
        }
    }
    // Attempt ordinary cleanup on every path, but an unobserved/unfinished
    // creation boundary remains a failed gate; cleanup is not acceptance.
    const bool closed=guard&&closeObservedTestWindow(guard.data());
    return settled&&closed&&engine.incubationController()==incubation&&incubation->incubatingObjectCount()==0;
}

class NativeWindowTest final : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        QTest::failOnWarning(QRegularExpression(".*(?:TypeError|ReferenceError):.*"));
        QTest::failOnWarning(QRegularExpression(".*Binding loop detected.*"));
        QTest::failOnWarning(QRegularExpression(".*items in the process of being created at engine destruction.*"));
        QTest::failOnWarning(QRegularExpression(".*(?:[Ff]ailed.*shader|shader.*[Ff]ailed|ShaderEffect.*(?:unsupported|not supported)).*"));
        QTest::failOnWarning(QRegularExpression(".*(?:Cannot open|Error decoding|Cannot load).*qrc:/workspace/.*"));
    }

    void focusedTextWindowLosesFocusBeforeNativeSurfaceIsDestroyed()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("focus-lifecycle.ini"));
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session);
        WindowFocusLifecycleProbe probe; // Outlives engine and every window callback.
        const auto dump=qScopeGuard([&] {
            if(QTest::currentTestFailed()) qInfo().noquote()<<"WINDOW_LIFECYCLE"<<QJsonDocument(probe.facts()).toJson(QJsonDocument::Compact);
        });
        {
            QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences);
            auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
            probe.watch(window);
            auto close=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
                if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Lifecycle witness window did not close",__FILE__,__LINE__);
            });
            QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
            QSignalSpy frame(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!frame.isEmpty());
            auto *field=item(window->contentItem(),"userInput"); QVERIFY(field && field->isVisible());
            QTest::mouseClick(window,Qt::LeftButton,{},field->mapToScene(QPointF(field->width()/2,field->height()/2)).toPoint());
            QTRY_VERIFY(field->hasActiveFocus()); QCOMPARE(QGuiApplication::focusWindow(),window);
            QVERIFY(window->activeFocusItem());
            QVERIFY(window->activeFocusItem()->flags().testFlag(QQuickItem::ItemAcceptsInputMethod));
            QVERIFY(!controller.authenticated()); QVERIFY(api.findChildren<QNetworkReply *>().isEmpty());
            probe.arm(window->winId()); // Already exposed: read the existing native ID.
            QVERIFY(closeObservedTestWindow(window)); close.dismiss();
            QVERIFY(probe.surfaceOrder()>0);
            QVERIFY2(probe.focusLostBeforeSurface(),"Observe inactive focus while the native surface still exists, before engine teardown");
        }
        QCOMPARE(probe.destroyedCount(),1);
        QVERIFY(!controller.authenticated()); QVERIFY(api.findChildren<QNetworkReply *>().isEmpty());
    }

    void focusedTextWindowEngineDestructionReleasesFocusBeforeSurface()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("engine-focus-lifecycle.ini"));
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session);
        WindowFocusLifecycleProbe probe; // Retains only public facts after the engine deletes its root.
        const auto dump=qScopeGuard([&] {
            if(QTest::currentTestFailed()) qInfo().noquote()<<"ENGINE_WINDOW_LIFECYCLE"<<QJsonDocument(probe.facts()).toJson(QJsonDocument::Compact);
        });
        {
            QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences);
            auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
            probe.watch(window);
            const auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
                if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Engine-owned lifecycle window did not close",__FILE__,__LINE__);
            });
            QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
            QSignalSpy frame(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!frame.isEmpty());
            auto *field=item(window->contentItem(),"userInput"); QVERIFY(field && field->isVisible());
            QTest::mouseClick(window,Qt::LeftButton,{},field->mapToScene(QPointF(field->width()/2,field->height()/2)).toPoint());
            QTRY_VERIFY(field->hasActiveFocus()); QCOMPARE(QGuiApplication::focusWindow(),window);
            QVERIFY(window->activeFocusItem());
            QVERIFY(window->activeFocusItem()->flags().testFlag(QQuickItem::ItemAcceptsInputMethod));
            QVERIFY(!controller.authenticated()); QVERIFY(api.findChildren<QNetworkReply *>().isEmpty());
            probe.arm(window->winId());
            // The guard runs before the engine's base destructor on normal
            // completion and every assertion return while Controller is alive.
        }
        QCOMPARE(probe.destroyedCount(),1);
        QVERIFY(probe.surfaceOrder()>0);
        QVERIFY2(probe.focusLostBeforeSurface(),"Observe focus loss before engine-owned native surface destruction");
        QVERIFY(!controller.authenticated()); QVERIFY(api.findChildren<QNetworkReply *>().isEmpty());
    }

    void messageTopicReferenceUsesTheCanonicalRoute_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"known", "unknown-same", "cross-canonical", "lookup-error", "cancel-pending"})
            QTest::newRow(name) << QString(name);
    }
    void messageTopicReferenceUsesTheCanonicalRoute()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("appearance.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        WindowTopicLinkFixture fixture(scenario);
        ApiClient api(nullptr, true); Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        const auto closeWindow = qScopeGuard([guard = QPointer<QQuickWindow>(window)] { if (guard) guard->close(); });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE_WITH_TIMEOUT(session.projects().size(), 1, 2000); session.selectProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(controller.channelMembersVerified(), 2000);
        // Stop polling only after the normal authenticated bootstrap has loaded
        // the verified directory; no realtime client exists in this fixture.
        controller.setRealtimeMode(true);
        controller.selectStream(streamId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.topics().size(), 2, 2000);
        controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && controller.messages().size() == 1, 2000);
        controller.setDraft("Preserve the original draft when opening fails");
        QTRY_VERIFY_WITH_TIMEOUT(visibleItem("message-content-" + messageId), 2000);
        auto *text = visibleItem("message-content-" + messageId);
        QSignalSpy frame(window, &QQuickWindow::frameSwapped);
        window->update(); QTRY_VERIFY_WITH_TIMEOUT(!frame.isEmpty(), 2000);
        const auto local = topicLinkPoint(text, fixture.href);
        QVERIFY2(local.x() >= 0, "A hit-tested displayed anchor must exist before input");
        QSignalSpy activated(text, SIGNAL(linkActivated(QString))); QVERIFY(activated.isValid());
        fixture.record = true;
        const auto point = text->mapToScene(local);
        QVERIFY(QRectF(0, 0, window->width(), window->height()).contains(point));
        QTest::mouseClick(window, Qt::LeftButton, {}, point.toPoint());
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.first().first().toString(), fixture.href);
        if (scenario == "lookup-error") {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.lookupGets, 1, 2000);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving(), 2000);
            QVERIFY(!controller.error().isEmpty());
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
            QCOMPARE(controller.draft(), "Preserve the original draft when opening fails");
            QCOMPARE(fixture.routeHistoryGets + fixture.detailGets + fixture.routeTopicGets, 0);
        } else if (scenario == "cancel-pending") {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.lookupGets, 1, 2000);
            QVERIFY(controller.topicResolving());
            QTRY_VERIFY_WITH_TIMEOUT(visibleItem("topic-" + secondTopicId), 2000);
            auto *sidebar = visibleItem("topic-" + secondTopicId);
            QSignalSpy chosen(sidebar, SIGNAL(clicked())); QVERIFY(chosen.isValid());
            click(window, sidebar); QCOMPARE(chosen.size(), 1);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving() && !controller.busy(), 2000);
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), secondTopicId);
            QCOMPARE(fixture.routeHistoryGets, 1); QCOMPARE(fixture.detailGets + fixture.routeTopicGets, 0);
            for (auto *reply : api.findChildren<QNetworkReply *>())
                QVERIFY(reply->url().path() != "/api/workspace/v1/messenger/stream_topics/" + linkRequestedId);
        } else {
            const auto expected = scenario == "known" ? secondTopicId : scenario == "cross-canonical" ? linkCanonicalId : linkRequestedId;
            QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), expected, 2000);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving() && !controller.busy(), 2000);
            QCOMPARE(controller.selectedStream(), scenario == "cross-canonical" ? linkDestinationId : streamId);
            QCOMPARE(fixture.lookupGets, scenario == "known" ? 0 : 1);
            QCOMPARE(fixture.detailGets, scenario == "cross-canonical" ? 1 : 0);
            QCOMPARE(fixture.routeTopicGets, scenario == "known" ? 0 : 1);
            QCOMPARE(fixture.routeHistoryGets, 1);
            QCOMPARE(controller.messages().first().toMap().value("id").toString(), returnedId);
            QVERIFY(controller.error().isEmpty());
        }
        QCOMPARE(fixture.aggregateGets, 0); QCOMPARE(fixture.mutations, 0);
        QVERIFY2(fixture.unexpected.isEmpty(), qPrintable(fixture.unexpected.join('\n')));
        for (const auto &target : fixture.historyTargets) {
            const auto query = QUrlQuery(target);
            QCOMPARE(query.queryItemValue("stream_uuid"), controller.selectedStream());
            QCOMPARE(query.queryItemValue("topic_uuid"), controller.selectedTopic());
        }
        window->close();
    }

    void directStreamCaptionRefreshesAfterDirectory_data()
    {
        QTest::addColumn<bool>("liveUpdate");
        QTest::newRow("late-directory") << false;
        QTest::newRow("live-rename") << true;
    }
    void directStreamCaptionRefreshesAfterDirectory()
    {
        QFETCH(bool, liveUpdate);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowDirectFixture fixture; fixture.seed();
        const auto original = fixture.server.handler;
        fixture.server.handler = [original](const Request &request) {
            auto response = original(request);
            if (request.target.path().endsWith("/users/")) response.delay = 150;
            return response;
        };
        ApiClient api(nullptr, true); Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 2);
        QTRY_VERIFY(controller.channelMembersVerified());
        const auto caption = [&] {
            for (const auto &entry : controller.streams())
                if (entry.toMap().value("id").toString() == windowDirectId())
                    return entry.toMap().value("displayName").toString();
            return QString{};
        };
        const auto visible = [&](const QString &label) {
            auto *row = item(window->contentItem(), "stream-"+windowDirectId());
            return row && hasVisibleText(row, label);
        };
        QCOMPARE(caption(), QString("Alice"));
        if (liveUpdate) {
            // Establish an already visible label through the normal refresh,
            // then isolate user.updated from the initial catalog response.
            controller.refresh();
            QTRY_VERIFY(controller.channelMembersVerified());
            QTRY_VERIFY(visible("Alice"));
            QJsonObject person{{"uuid",channelPeer},{"username","alice"},{"first_name","Alicia"},
                {"email","alice@example.invalid"},{"kind","user.updated"}};
            QVERIFY(controller.applyRealtimeEvent(QJsonObject{{"uuid",returnedId},{"project_id",projectId},
                {"user_uuid",userId},{"payload",person}}));
            QCOMPARE(caption(), QString("Alicia"));
        }
        QTRY_VERIFY2(visible(liveUpdate ? "Alicia" : "Alice"),
            "Visible sidebar caption must follow the verified directory, as does the controller getter");
        QCOMPARE(fixture.posts, 0); window->close();
    }

    void realMenuInputWaitsForSourceLayout()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        MockServer server;
        int mutations=0;
        server.handler=[&](const Request &request)->Response {
            const auto path=request.target.path();
            if(request.method!="GET" && !path.contains("/iam/")) ++mutations;
            if(path.endsWith("/streams/")) return {QJsonArray{
                QJsonObject{{"uuid",streamId},{"project_id",projectId},{"name","Product team"},{"encryption",false},{"role","member"}},
                QJsonObject{{"uuid","88888888-8888-4888-8888-888888888888"},{"project_id",projectId},{"name","<b>Other team</b>"},{"encryption",false}}}};
            if(path.endsWith("/users/") || path.endsWith("/messages/")) return {QJsonArray{}};
            if(path.endsWith("/stream_topics/")) return {QJsonArray{QJsonObject{{"uuid",topicId},{"project_id",projectId},{"stream_uuid",streamId},{"user_uuid",userId},{"name","Native desktop"},{"is_done",false},{"is_default",false},{"unread_count",0},{"active_unread_count",0},{"passive_unread_count",0},{"last_message_uuid",QJsonValue::Null}}}};
            return sessionResponse(request);
        };
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session); QQmlApplicationEngine engine;
        loadNativeUi(engine,&controller,&preferences); auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(server.origin(),"synthetic-user","synthetic-password"); QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(),2);controller.selectStream(streamId);QTRY_COMPARE(controller.topics().size(),1);controller.selectTopic(topicId);QTRY_VERIFY(controller.canCreateTopicIn(streamId));
        auto *menu=window->findChild<QObject *>("streamContextMenu"); QVERIFY(menu);
        auto *list=menu->property("contentItem").value<QQuickItem *>(); QVERIFY(list);
        auto *action=window->findChild<QQuickItem *>("streamNewTopicAction"); QVERIFY(action);
        auto *stream=item(window->contentItem(),"stream-"+streamId); QVERIFY(stream);
        const auto point=stream->mapToScene(QPointF(20,stream->height()/2));
        QVERIFY(QMetaObject::invokeMethod(window,"openStreamMenu",Q_ARG(QVariant,QVariant(streamId)),Q_ARG(QVariant,QVariant::fromValue(static_cast<QObject *>(stream))),Q_ARG(QVariant,QVariant(point))));
        QCOMPARE(menu->property("height").toReal(),qreal(10));
        QCOMPARE(list->property("contentHeight").toReal(),qreal(0));
        QCOMPARE(action->height(),qreal(36));
        // No artificial delay: guard must observe actual complete source layout
        // before the very same real-window input used by the interaction helper.
        awaitTopicMenuLayout(window,action,streamId);
        const auto heightAtPress=menu->property("height").toReal();
        const auto clickPoint=action->mapToScene(QPointF(action->width()/2,action->height()/2));
        qInfo()<<"Controlled menu height at real press"<<heightAtPress<<clickPoint;
        QTest::mouseClick(window,Qt::LeftButton,{},clickPoint.toPoint());
        QTRY_VERIFY(visibleItem("topicNameInput"));
        QCOMPARE(heightAtPress,qreal(46));
        QCOMPARE(mutations,0);window->close();
    }

    void selectedStreamEncryptionProjectsEachNeededModelOnce_data()
    {
        QTest::addColumn<QString>("selection");
        QTest::addColumn<QString>("location");
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<int>("activeReads");
        QTest::addColumn<int>("archiveReads");
        QTest::newRow("empty-505") << QString{} << QString("none") << true << 0 << 0;
        QTest::newRow("active-last-plain-505") << QString("target") << QString("active") << false << 1 << 0;
        QTest::newRow("active-last-encrypted-505") << QString("target") << QString("active") << true << 1 << 0;
        QTest::newRow("archive-last-plain-505") << QString("target") << QString("archive") << false << 1 << 1;
        QTest::newRow("archive-last-encrypted-505") << QString("target") << QString("archive") << true << 1 << 1;
        QTest::newRow("unknown-505") << QString("missing") << QString("none") << true << 1 << 1;
    }
    void selectedStreamEncryptionProjectsEachNeededModelOnce()
    {
        QFETCH(QString,selection); QFETCH(QString,location); QFETCH(bool,encrypted);
        QFETCH(int,activeReads); QFETCH(int,archiveReads);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("p.ini"));
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        // Exercise the production QML function with JS getter boundaries. The
        // displayed application still uses its real controller and no mock model.
        const auto config=QString::fromUtf8(QJsonDocument(QJsonObject{{"selection",selection},
            {"location",location},{"encrypted",encrypted}}).toJson(QJsonDocument::Compact));
        const auto script=QString(R"JS((function() {
            const config=%1;
            const active=[], archived=[];
            for(let i=0;i<505;++i) {
                active.push({id:"active-"+i,encrypted:false});
                archived.push({id:"archive-"+i,encrypted:false});
            }
            if(config.location==="active") active[504]={id:"target",encrypted:config.encrypted};
            if(config.location==="archive") archived[504]={id:"target",encrypted:config.encrypted};
            const calls={selection:0,active:0,archive:0};
            const provider={
                get selectedStream() { ++calls.selection; return config.selection; },
                get streams() { ++calls.active; return active; },
                get archivedChats() { ++calls.archive; return archived; }
            };
            const result=selectedStreamIsEncrypted(provider);
            return {encrypted:result,selectionReads:calls.selection,
                    activeReads:calls.active,archiveReads:calls.archive};
        })())JS").arg(config);
        QQmlExpression expression(engine.rootContext(),window,script);
        auto value=expression.evaluate();
        QVERIFY2(!expression.hasError(),qPrintable(expression.error().toString()));
        if(value.metaType()==QMetaType::fromType<QJSValue>()) value=value.value<QJSValue>().toVariant();
        const auto result=value.toMap(); QVERIFY(!result.isEmpty());
        qInfo()<<"Production QML getter counts"<<QTest::currentDataTag()<<result;
        QCOMPARE(result.value("encrypted").toBool(),encrypted);
        QCOMPARE(result.value("activeReads").toInt(),activeReads);
        QCOMPARE(result.value("archiveReads").toInt(),archiveReads);
        QVERIFY(result.value("selectionReads").toInt()<=1);
        QCOMPARE(window->property("selectedStreamEncrypted").toBool(),true);
        window->close();
    }

    void archiveMatchesPinnedGeometryAndVirtualSearch_data() {
        QTest::addColumn<QString>("scenario");
        for(const auto *name:{"en-light","en-dark","ru-light","ru-dark","ru-light-700","ru-light-600","short","virtual"}) QTest::newRow(name)<<QString(name);
    }
    void archiveMatchesPinnedGeometryAndVirtualSearch() {
        QFETCH(QString,scenario); QTemporaryDir directory; NativePreferences preferences(directory.filePath("p.ini"));
        preferences.setLocale(scenario.startsWith("ru")?"ru":"en");preferences.setMode(scenario.contains("dark")?"dark":"light");
        WindowArchiveFixture fixture;
        if(scenario=="short") fixture.archived=QJsonArray{fixture.archived.first()};
        if(scenario=="virtual") for(int n=6;n<=505;++n) {auto row=windowChannel(archiveRowId(n),QString("Virtual archive %1").arg(n));row["is_archived"]=true;fixture.archived.append(row);}
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        if(scenario.endsWith("700"))window->resize(700,600);else if(scenario.endsWith("600")){window->setMinimumWidth(0);window->resize(600,600);}
        QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);
        QTRY_VERIFY(controller.channelMembersVerified());QTRY_VERIFY(!controller.archiveLoading());
        controller.selectStream(streamId);QTRY_COMPARE(controller.messages().size(),7);QTRY_COMPARE(controller.topics().size(),3);
        openArchive(window);auto *search=visibleItem("archiveChatSearch");auto *list=visibleItem("archiveChatList");auto *roster=visibleItem("archiveChatRoster");auto *dialog=window->findChild<QObject *>("createChatDialog");QVERIFY(search&&list&&roster&&dialog);
        QTRY_COMPARE(dialog->property("height").toReal(),qreal(512));QCOMPARE(roster->height(),qreal(240));QCOMPARE(search->height(),qreal(40.84375));
        const auto narrow=scenario.endsWith("700")||scenario.endsWith("600");
        QTRY_COMPARE(search->mapToScene(QPointF{}),narrow?QPointF(17,scenario.endsWith("600")?147:151):QPointF(289,301));
        auto *first=visibleItem("archive-open-"+archiveWindowId);auto *restore=visibleItem("archive-restore-"+archiveWindowId);QVERIFY(first&&restore);
        QTRY_COMPARE(first->height(),qreal(51.703125));QCOMPARE(restore->size(),QSizeF(32,32));
        QTRY_COMPARE(first->width(),roster->width()-(scenario=="short"?42:52));
        QCOMPARE(visibleItem("archive-name-"+archiveWindowId)->property("text").toString(),QString("Alice"));
        QCOMPARE(visibleItem("archive-name-"+archiveWindowId)->property("textFormat").toInt(),int(Qt::PlainText));
        const auto catalogs=fixture.catalogGets,users=fixture.usersGets,history=fixture.historyGets;
        click(window,search);
        const auto capture=[&](const QString &state) { if(scenario!="virtual")captureArchiveWindow(window,scenario,state); };
        capture("normal");
        if(scenario=="virtual"){QVERIFY(itemCountWithPrefix(window->contentItem(),"archive-row-")<30);QCOMPARE(controller.archivedChats().size(),505);}
        for(const auto key:QByteArray("  ALICE  "))QTest::keyClick(window,key);QTRY_COMPARE(list->property("count").toInt(),1);QVERIFY(visibleItem("archive-open-"+archiveWindowId));
        click(window,visibleItem("createChatTab-0"));click(window,visibleItem("createChatTab-3"));QCOMPARE(search->property("text").toString(),QString("  ALICE  "));
        click(window,search);QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);for(const auto key:QByteArray("zzzz-no-synthetic-chat"))QTest::keyClick(window,key);QTRY_COMPARE(list->property("count").toInt(),0);QVERIFY(visibleItem("archiveEmptyStatus"));capture("empty");
        QCOMPARE(fixture.catalogGets,catalogs);QCOMPARE(fixture.usersGets,users);QCOMPARE(fixture.historyGets,history);QCOMPARE(fixture.restorePosts,0);
        QTest::keyClick(window,Qt::Key_Escape);QTRY_VERIFY(!visibleItem("archiveChatSearch"));openArchive(window);QVERIFY(visibleItem("archiveChatSearch")->property("text").toString().isEmpty());window->close();
    }
    void archiveUsesExactHistoryAndCanonicalRestore_data() {
        QTest::addColumn<QString>("scenario");QTest::addColumn<QString>("localeTheme");for(const auto *name:{"open-direct","open-channel","open-ordinary-topic","open-encrypted","open-encrypted-topic","open-provider","restore","error-retry","ambiguous","open-pending","late-error"})QTest::newRow(name)<<QString(name)<<QString("en-light");
        QTest::newRow("ru-dark-error-retry")<<QString("error-retry")<<QString("ru-dark");QTest::newRow("ru-dark-open-pending")<<QString("open-pending")<<QString("ru-dark");
    }
    void archiveUsesExactHistoryAndCanonicalRestore() {
        QFETCH(QString,scenario);QFETCH(QString,localeTheme);QTemporaryDir directory;NativePreferences preferences(directory.filePath("p.ini"));preferences.setLocale(localeTheme.startsWith("ru")?"ru":"en");preferences.setMode(localeTheme.endsWith("dark")?"dark":"light");WindowArchiveFixture fixture;
        if(scenario=="error-retry"||scenario=="late-error"){fixture.restoreStatus=403;fixture.applyRestore=false;}
        if(scenario=="ambiguous"){fixture.restoreStatus=fixture.restoreGetStatus=503;fixture.applyRestore=false;}
        fixture.restoreDelay=300;
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);FileClient files(&api);controller.useFiles(&files);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);QTRY_VERIFY(controller.channelMembersVerified());
        if(!qEnvironmentVariableIsEmpty("CASSI_NATIVE_ARCHIVE_SCREENSHOT_DIRECTORY")){controller.selectStream(streamId);QTRY_COMPARE(controller.messages().size(),7);QTRY_COMPARE(controller.topics().size(),3);}openArchive(window);
        const auto id=(scenario=="open-channel"||scenario=="open-ordinary-topic")?archiveRowId(2):(scenario=="open-encrypted"||scenario=="open-encrypted-topic")?archiveRowId(4):scenario=="open-provider"?archiveRowId(5):archiveWindowId;
        if(scenario.startsWith("open-")&&scenario!="open-pending") {
            if(id==archiveRowId(5)){auto *list=visibleItem("archiveChatList");QVERIFY(QMetaObject::invokeMethod(list,"positionViewAtEnd"));}
            QTRY_VERIFY(visibleItem("archive-open-"+id));click(window,visibleItem("archive-open-"+id));QTRY_VERIFY(!visibleItem("archiveChatSearch"));
            QCOMPARE(controller.selectedStream(),id);QCOMPARE(controller.conversationKind(),QString("stream"));QVERIFY(controller.selectedTopic().isEmpty());QVERIFY(!controller.canSend());QVERIFY(!controller.canAttach());QCOMPARE(fixture.restorePosts,0);QCOMPARE(fixture.restoreGets,0);
            if(scenario=="open-ordinary-topic"||scenario=="open-encrypted-topic") {
                const bool encrypted=scenario=="open-encrypted-topic";
                if (encrypted) {
                    // Source gives the encrypted footer precedence over the
                    // ordinary shortcut rail. The first aggregate message has
                    // a topic caption; dividers begin only at the second run.
                    QVERIFY(!visibleItem("streamTopicPrompt"));
                    QTRY_VERIFY(visibleItem("message-topic-" + messageId));
                    auto *topicLink = visibleItem("message-topic-" + messageId);
                    QSignalSpy activated(topicLink, SIGNAL(clicked()));
                    QVERIFY(activated.isValid());
                    click(window, topicLink);
                    QCOMPARE(activated.size(), 1);
                } else {
                    QTRY_VERIFY(visibleItem("topicShortcut-"+topicId));
                    click(window,visibleItem("topicShortcut-"+topicId));
                }
                QTRY_VERIFY(!controller.busy());
                QCOMPARE(controller.selectedTopic(),topicId);controller.setDraft("Archived topic draft");QCOMPARE(controller.canSend(),!encrypted);QCOMPARE(controller.canAttach(),!encrypted);
                QCOMPARE(window->property("selectedStreamEncrypted").toBool(),encrypted);
                if (encrypted) {
                    // The source encrypted read footer replaces its unsupported
                    // composer, while the core send/upload guards remain closed.
                    QVERIFY(visibleItem("encryptedReadStatus"));
                    QVERIFY(!visibleItem("messageComposer"));QVERIFY(!visibleItem("attachButton"));
                    QVERIFY(!controller.canSend());QVERIFY(!controller.canAttach());
                } else {
                    auto *composer=visibleItem("messageComposer");QVERIFY(composer);QVERIFY(composer->isEnabled());
                    auto *attach=visibleItem("attachButton");QVERIFY(attach);QVERIFY(attach->isEnabled());
                }
            }
            window->close();return;
        }
        click(window,visibleItem("archive-restore-"+id));QTRY_COMPARE(fixture.restorePosts,1);QVERIFY(!visibleItem("archive-restore-"+id)->isEnabled());QVERIFY(visibleItem("archive-open-"+id)->isEnabled());
        if(scenario=="open-pending"){captureArchiveWindow(window,localeTheme,"pending");click(window,visibleItem("archive-open-"+id));QTRY_VERIFY(!visibleItem("archiveChatSearch"));QCOMPARE(controller.selectedStream(),id);QTRY_COMPARE(controller.archivedChats().size(),4);QCOMPARE(fixture.restorePosts,1);window->close();return;}
        if(scenario=="late-error"){click(window,visibleItem("channelCancelButton"));openArchive(window);QTRY_VERIFY(controller.canUnarchiveChat(id));QVERIFY(!visibleItem("archiveRestoreAlert"));QCOMPARE(fixture.restorePosts,1);window->close();return;}
        if(scenario=="restore"){QTRY_COMPARE(controller.archivedChats().size(),4);QVERIFY(visibleItem("archiveChatSearch"));QVERIFY(controller.selectedStream().isEmpty());QCOMPARE(fixture.restorePosts,1);QCOMPARE(fixture.restoreGets,0);window->close();return;}
        QTRY_VERIFY(visibleItem("archiveRestoreAlert"));QCOMPARE(visibleItem("archiveRestoreAlert")->height(),qreal(38));
        QTRY_COMPARE(window->findChild<QObject *>("createChatDialog")->property("height").toReal(),qreal(515.84375));
        QTRY_COMPARE(visibleItem("archiveChatRoster")->mapToScene(QPointF{}),QPointF(289,401.921875));
        if(scenario=="error-retry")captureArchiveWindow(window,localeTheme,"error");
        QVERIFY(visibleItem("archive-restore-"+id)->isEnabled());QCOMPARE(controller.archivedChats().size(),5);QCOMPARE(fixture.restorePosts,1);
        fixture.restoreStatus=fixture.restoreGetStatus=200;fixture.applyRestore=true;
        click(window,visibleItem("archive-restore-"+id));QTRY_COMPARE(controller.archivedChats().size(),4);QVERIFY(!visibleItem("archiveRestoreAlert"));QCOMPARE(fixture.restorePosts,2);QCOMPARE(fixture.restoreGets,scenario=="ambiguous"?2:0);window->close();
    }
    void archiveCatalogStatesKeepRowsAndDistinguishLoadingFromFailure_data() {
        QTest::addColumn<QString>("scenario");for(const auto *name:{"loading-empty","loading-retained","error-empty","error-retained"})QTest::newRow(name)<<QString(name);
    }
    void archiveCatalogStatesKeepRowsAndDistinguishLoadingFromFailure() {
        QFETCH(QString,scenario);QTemporaryDir directory;NativePreferences preferences(directory.filePath("p.ini"));preferences.setLocale("en");WindowArchiveFixture fixture;
        const bool retained=scenario.endsWith("retained"),failure=scenario.startsWith("error");
        if(!retained)fixture.archived=QJsonArray{};
        if(!retained){fixture.archiveCatalogDelay=700;if(failure)fixture.archiveCatalogStatus=503;}
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);
        QTRY_VERIFY(controller.authenticated());
        if(retained){QTRY_VERIFY(controller.channelMembersVerified());QTRY_VERIFY(!controller.archiveLoading());}
        openArchive(window);
        if(retained){fixture.archiveCatalogDelay=700;if(failure)fixture.archiveCatalogStatus=503;controller.refresh();}
        QTRY_VERIFY(controller.archiveLoading());QVERIFY(visibleItem("archiveChatSearch"));
        QCOMPARE(visibleItem("archiveChatList")->property("count").toInt(),retained?5:0);
        QCOMPARE(bool(visibleItem("archiveLoadingStatus")),!retained);QVERIFY(!visibleItem("archiveEmptyStatus"));
        if(retained)QVERIFY(!visibleItem("archive-restore-"+archiveWindowId)->isEnabled());
        QTRY_VERIFY(!controller.archiveLoading());QVERIFY(visibleItem("archiveChatSearch"));
        if(failure){
            QVERIFY(visibleItem("archiveCatalogError"));QVERIFY(!visibleItem("archiveEmptyStatus"));
            QTRY_COMPARE(visibleItem("archiveChatRoster")->mapToScene(QPointF{}),QPointF(289,385.84375));
            QCOMPARE(window->findChild<QObject *>("createChatDialog")->property("height").toReal(),qreal(512));
        }else{
            QVERIFY(!visibleItem("archiveCatalogError"));QCOMPARE(bool(visibleItem("archiveEmptyStatus")),!retained);
        }
        QCOMPARE(fixture.restorePosts,0);window->close();
    }

    void archiveDifferentRowFailuresUseTheCurrentViewAndCompletionOrder() {
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("p.ini"));WindowArchiveFixture fixture;
        fixture.applyRestore=false;fixture.restoreStatus=403;fixture.rowDelays[archiveWindowId]=450;fixture.rowDelays[archiveRowId(2)]=220;
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);QTRY_VERIFY(controller.channelMembersVerified());openArchive(window);
        click(window,visibleItem("archive-restore-"+archiveWindowId));click(window,visibleItem("archive-restore-"+archiveRowId(2)));QTRY_COMPARE(fixture.restorePosts,2);
        QTRY_COMPARE(window->property("archiveErrorTarget").toString(),archiveRowId(2));QTRY_COMPARE(window->property("archiveErrorTarget").toString(),archiveWindowId);
        QCOMPARE(controller.archivedChats().size(),5);QVERIFY(visibleItem("archiveRestoreAlert"));
        click(window,visibleItem("channelCancelButton"));openArchive(window);QVERIFY(!visibleItem("archiveRestoreAlert"));
        QVERIFY(window->property("archiveErrorTarget").toString().isEmpty());QCOMPARE(fixture.restorePosts,2);window->close();
    }

    void archiveKeyboardAndOwnerLifetimeRemainScoped() {
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("p.ini"));WindowArchiveFixture fixture;ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);QTRY_VERIFY(controller.channelMembersVerified());
        openArchive(window);auto *tab=visibleItem("createChatTab-3");tab->forceActiveFocus(Qt::TabFocusReason);QTest::keyClick(window,Qt::Key_Right);QTRY_COMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(),0);QTest::keyClick(window,Qt::Key_Left);QTRY_COMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(),3);
        QTest::keyClick(window,Qt::Key_Tab);QTRY_VERIFY(visibleItem("archiveChatSearch")->hasActiveFocus());QTest::keyClick(window,Qt::Key_Tab);QTRY_VERIFY(visibleItem("archive-open-"+archiveWindowId)->hasActiveFocus());QTest::keyClick(window,Qt::Key_Tab);QTRY_VERIFY(visibleItem("archive-restore-"+archiveWindowId)->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Tab,Qt::ShiftModifier);QTRY_VERIFY(visibleItem("archive-open-"+archiveWindowId)->hasActiveFocus());
        session.logout();QTRY_VERIFY(!visibleItem("archiveChatSearch"));QVERIFY(window->property("archiveQuery").toString().isEmpty());QVERIFY(window->property("archiveErrorTarget").toString().isEmpty());QVERIFY(controller.archivedChats().isEmpty());window->close();
    }

    void archiveScrollbarMatchesPinnedPaintAndKeepsRealScrolling_data() {
        QTest::addColumn<bool>("dark");QTest::addColumn<int>("rowCount");QTest::newRow("light")<<false<<5;QTest::newRow("dark")<<true<<5;QTest::newRow("large")<<false<<505;
    }
    void archiveScrollbarMatchesPinnedPaintAndKeepsRealScrolling() {
        QFETCH(bool,dark);QFETCH(int,rowCount);QTemporaryDir directory;NativePreferences preferences(directory.filePath("p.ini"));preferences.setLocale("en");preferences.setMode(dark?"dark":"light");
        WindowArchiveFixture fixture;for(int n=6;n<=rowCount;++n){auto row=windowChannel(archiveRowId(n),QString("Virtual archive %1").arg(n));row["is_archived"]=true;fixture.archived.append(row);}ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);QTRY_VERIFY(controller.channelMembersVerified());openArchive(window);
        auto *roster=visibleItem("archiveChatRoster");auto *list=visibleItem("archiveChatList");QVERIFY(roster&&list);QTRY_VERIFY(window->findChild<QObject *>("createChatDialog")->property("opened").toBool());QTest::mouseMove(window,QPoint(15,window->height()-15));
        QSignalSpy frame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!frame.isEmpty());
        auto *bar=visibleItem("archiveChatScrollBar");QVERIFY(bar);QCOMPARE(bar->width(),qreal(10));QCOMPARE(bar->property("topPadding").toReal(),qreal(12));QCOMPARE(bar->property("bottomPadding").toReal(),qreal(12));
        auto *thumbItem=qobject_cast<QQuickItem *>(bar->property("contentItem").value<QObject *>());QVERIFY(thumbItem);QCOMPARE(thumbItem->width(),qreal(6));
        if(rowCount==5){QVERIFY(thumbItem->height()>196.5&&thumbItem->height()<198);QCOMPARE(thumbItem->mapToItem(bar,QPointF{}).y(),qreal(12));}
        const auto origin=roster->mapToScene(QPointF{});const auto image=window->grabWindow();QVERIFY(!image.isNull());const auto ratio=window->devicePixelRatio();
        const auto pixel=[&](qreal x,qreal y){return image.pixelColor(qFloor((origin.x()+x)*ratio),qFloor((origin.y()+y)*ratio));};
        const auto thumb=QColor(dark?"#33384d":"#d8e4ef"),track=QColor(dark?"#222328":"#ffffff");
        // Original Chromium frames:10px gutter,6px palette thumb and12px end controls.
        if(rowCount==5){QCOMPARE(pixel(856,50),thumb);QCOMPARE(pixel(856,10),track);QCOMPARE(pixel(856,7),thumb);QCOMPARE(pixel(856,233),thumb);}
        const auto contentY=list->property("contentY").toReal();
        const auto down=roster->mapToScene(QPointF(856,233)).toPoint();
        if(rowCount>5){QTest::mousePress(window,Qt::LeftButton,{},down);QTRY_VERIFY(list->property("contentY").toReal()>contentY);QTest::mouseRelease(window,Qt::LeftButton,{},down);QVERIFY(itemCountWithPrefix(window->contentItem(),"archive-row-")<30);}
        else{QTest::mouseClick(window,Qt::LeftButton,{},down);QTRY_VERIFY(list->property("contentY").toReal()>contentY);}
        const auto atBottom=list->property("contentY").toReal();QTest::mouseClick(window,Qt::LeftButton,{},roster->mapToScene(QPointF(856,6)).toPoint());QTRY_VERIFY(list->property("contentY").toReal()<atBottom);
        auto *accessible=QAccessible::queryAccessibleInterface(visibleItem("archiveChatScrollBar"));QVERIFY(accessible);QCOMPARE(accessible->role(),QAccessible::ScrollBar);
        auto *actions=accessible->actionInterface();QVERIFY(actions);QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::increaseAction()));QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::decreaseAction()));
        QCOMPARE(fixture.restorePosts,0);window->close();
    }

    void archiveErrorAtMinimumWindowKeepsFooterAndBodyReachable() {
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("p.ini"));preferences.setLocale("ru");WindowArchiveFixture fixture;fixture.restoreStatus=403;fixture.applyRestore=false;
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);window->resize(700,600);
        QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);QTRY_VERIFY(controller.channelMembersVerified());openArchive(window);
        click(window,visibleItem("archive-restore-"+archiveWindowId));QTRY_VERIFY(visibleItem("archiveRestoreAlert"));auto *body=visibleItem("archiveChatBody");auto *cancel=visibleItem("channelCancelButton");QVERIFY(body&&cancel);
        QTRY_VERIFY(body->property("contentHeight").toReal()>body->height());const auto footer=cancel->mapToScene(QPointF{});QVERIFY(footer.y()>=0&&footer.y()+cancel->height()<=window->height());
        // QTest mouse timestamps include a synthetic post-release gap; the wheel
        // helper uses elapsed wall time and can be rejected by Flickable as older.
        // Send one chronological programmatic window event, without a real delay.
        const auto point=body->mapToScene(QPointF(5,30)).toPoint();
        QWheelEvent wheel(point,window->mapToGlobal(point),{},QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        wheel.setTimestamp(QTest::lastMouseTimestamp+1);QCoreApplication::sendEvent(window,&wheel);
        QTRY_VERIFY(body->property("contentY").toReal()>0);QCOMPARE(cancel->mapToScene(QPointF{}),footer);
        click(window,cancel);QTRY_VERIFY(!visibleItem("archiveChatSearch"));QCOMPARE(fixture.restorePosts,1);window->close();
    }

    void archiveIndicatorsMatchPinnedSmallRingAndStopWhenHidden_data() {
        QTest::addColumn<bool>("pending");QTest::addColumn<bool>("dark");
        QTest::newRow("pending-light")<<true<<false;QTest::newRow("pending-dark")<<true<<true;
        QTest::newRow("loading-light")<<false<<false;QTest::newRow("loading-dark")<<false<<true;
    }
    void archiveIndicatorsMatchPinnedSmallRingAndStopWhenHidden() {
        QFETCH(bool,pending);QFETCH(bool,dark);QTemporaryDir directory;NativePreferences preferences(directory.filePath("p.ini"));preferences.setLocale("en");preferences.setMode(dark?"dark":"light");
        WindowArchiveFixture fixture;fixture.applyRestore=false;fixture.restoreStatus=403;fixture.restoreDelay=700;
        if(!pending){fixture.archived=QJsonArray{};fixture.archiveCatalogDelay=700;}
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);QTRY_VERIFY(controller.authenticated());if(pending)QTRY_VERIFY(controller.channelMembersVerified());openArchive(window);
        if(pending)click(window,visibleItem("archive-restore-"+archiveWindowId));
        const auto name=pending?"archiveRestoreSpinner-"+archiveWindowId:QString("archiveLoadingSpinner");QTRY_VERIFY(visibleItem(name));auto *spinner=visibleItem(name);
        QTest::mouseMove(window,QPoint(15,window->height()-15));QSignalSpy frame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!frame.isEmpty());
        const auto image=window->grabWindow();QVERIFY(!image.isNull());const auto center=spinner->mapToScene(QPointF(spinner->width()/2,spinner->height()/2));const auto ratio=window->devicePixelRatio();
        const auto background=QColor(dark?"#222328":"#ffffff");QRect ink;
        for(int y=-9;y<=9;++y)for(int x=-9;x<=9;++x){const auto pixel=image.pixelColor(qFloor((center.x()+x)*ratio),qFloor((center.y()+y)*ratio));if(qAbs(pixel.red()-background.red())+qAbs(pixel.green()-background.green())+qAbs(pixel.blue()-background.blue())>5)ink|=QRect(x,y,1,1);}
        if(spinner->size()!=QSizeF(14,14)||ink.width()<12||ink.height()<12)qWarning()<<"Pinned sm14px ring vs actual item/ink"<<spinner->size()<<ink;
        QCOMPARE(spinner->size(),QSizeF(14,14));QVERIFY(ink.width()>=12&&ink.height()>=12);
        QPointer<QQuickItem> guarded(spinner);click(window,visibleItem("channelCancelButton"));QTRY_VERIFY(!visibleItem("archiveChatSearch"));QVERIFY(guarded);QVERIFY(!guarded->property("spinning").toBool());window->close();
    }

    void globalNewChatOffersTheSourceArchiveTab()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowChannelFixture fixture;
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(),"synthetic","synthetic"); QTRY_COMPARE(session.projects().size(),1);
        session.selectProject(projectId); QTRY_VERIFY(controller.channelMembersVerified());
        click(window,item(window->contentItem(),"createChatButton"));
        auto *tab=visibleItem("createChatTab-3"); QVERIFY(tab);
        QVERIFY2(tab->isEnabled(),"Pinned source enables the fourth Archive tab");
        click(window,tab); QTRY_VERIFY(visibleItem("archiveChatSearch"));
        QCOMPARE(fixture.posts,0); window->close();
    }

    void globalNewChatOffersTheSourceTopicTab()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowChannelFixture fixture;
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateTopicIn(streamId));
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("directChatSearch"));
        auto *tab = visibleItem("createChatTab-2"); QVERIFY(tab);
        QVERIFY2(tab->isEnabled(), "Source global New chat enables the Create topic tab");
        click(window, tab);
        QTRY_VERIFY(visibleItem("globalTopicStreamSelector"));
        QTRY_VERIFY(visibleItem("globalTopicNameInput"));
        QCOMPARE(fixture.posts, 0); window->close();
    }

    void globalTopicUsesTypedHttpForEveryMetadataKind_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *name : {"ordinary", "direct", "encrypted", "provider", "uncertain", "shared-sidebar"}) QTest::newRow(name) << QString(name);
    }
    void globalTopicUsesTypedHttpForEveryMetadataKind()
    {
        QFETCH(QString, kind);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini")); preferences.setLocale("en");
        WindowTopicFixture fixture;
        if (kind == "uncertain" || kind == "shared-sidebar") { fixture.postStatus = 503; fixture.canonicalStatus = 404; }
        const QString target = kind == "direct" ? QString("88000000-0000-4000-8000-000000000001")
            : kind == "encrypted" ? QString("88000000-0000-4000-8000-000000000002")
            : kind == "provider" ? QString("88000000-0000-4000-8000-000000000003") : streamId;
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 4);
        QTRY_VERIFY(controller.canCreateTopicIn(target));
        if (kind == "shared-sidebar") {
            openStreamTopicForm(window, target);
            visibleItem("topicNameInput")->setProperty("text", "Native metadata topic");
            click(window, visibleItem("topicFormSubmit"));
            QTRY_VERIFY(controller.topicCreationUncertain());
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!visibleItem("topicNameInput"));
            click(window, item(window->contentItem(), "createChatButton"));
            QTRY_VERIFY(visibleItem("directChatSearch"));
            QCOMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(), 0);
            click(window, visibleItem("createChatTab-2"));
        } else openGlobalTopic(window);
        auto *selector = visibleItem("globalTopicStreamSelector");
        auto *name = visibleItem("globalTopicNameInput");
        auto *primary = visibleItem("globalTopicCreateButton");
        QVERIFY(selector && name && primary);
        QCOMPARE(window->property("globalTopicStreams").toList().size(), 4);
        if (kind != "shared-sidebar") {
            click(window, selector);
            QTRY_VERIFY(visibleItem("globalTopicOption-" + target));
            QCOMPARE(visibleItem("globalTopicOption-" + target)->property("modelData").toMap()["name"].toString(), kind == "direct" ? QString("Personal chat") : kind == "encrypted" ? QString("Encrypted channel") : kind == "provider" ? QString("Provider channel") : QString("Product team"));
            click(window, visibleItem("globalTopicOption-" + target));
            name->setProperty("text", "  Native metadata topic  ");
            name->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Return);
            QCOMPARE(fixture.posts, 0); // Source normal name Enter does not submit.
            click(window, primary);
        }
        QTRY_COMPARE(fixture.posts, 1);
        const auto attempt = controller.topicCreationAttemptId();
        QCOMPARE(fixture.posted, QJsonObject({{"uuid", attempt}, {"stream_uuid", target}, {"name", "Native metadata topic"}}));
        if (kind == "uncertain" || kind == "shared-sidebar") {
            QTRY_VERIFY(controller.topicCreationUncertain());
            QVERIFY(!selector->isEnabled());
            QCOMPARE(window->property("globalTopicStreams").toList().size(), 4);
            QCOMPARE(primary->property("text").toString(), QString("Check status"));
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!visibleItem("globalTopicNameInput"));
            click(window, item(window->contentItem(), "createChatButton"));
            QTRY_VERIFY(visibleItem("globalTopicNameInput"));
            QCOMPARE(window->property("globalTopicAttempt").toString(), attempt);
            QCOMPARE(visibleItem("globalTopicNameInput")->property("text").toString(), QString("Native metadata topic"));
            click(window, visibleItem("globalTopicCreateButton"));
            QTRY_COMPARE(fixture.confirmations, 1); QTRY_VERIFY(!controller.topicCreationBusy());
            QVERIFY(controller.topicCreationUncertain()); QCOMPARE(fixture.posts, 1);
            fixture.canonicalStatus = 200;
            click(window, visibleItem("globalTopicCreateButton"));
        }
        QTRY_COMPARE(controller.selectedTopic(), attempt);
        QCOMPARE(controller.selectedStream(), target);
        QTRY_VERIFY(!visibleItem("globalTopicNameInput"));
        QCOMPARE(fixture.posts, 1); QCOMPARE(fixture.catalogs, 1);
        QCOMPARE(fixture.confirmations, kind == "uncertain" || kind == "shared-sidebar" ? 2 : 1);
        window->close();
    }

    void globalChatTabsUseSourceIntrinsicPressureAndBreakpoint_data()
    {
        QTest::addColumn<int>("width"); QTest::addColumn<int>("tab"); QTest::addColumn<qreal>("height");
        for (int width : {700, 600}) for (int tab : {0, 1, 2})
            QTest::newRow(qPrintable(QString("%1-%2").arg(width).arg(tab))) << width << tab << qreal(tab == 1 ? width == 700 ? 26.015625 : 23.515625 : width == 700 ? 39 : 35);
    }
    void globalChatTabsUseSourceIntrinsicPressureAndBreakpoint()
    {
        QFETCH(int, width); QFETCH(int, tab); QFETCH(qreal, height);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowChannelFixture fixture; ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        // 600px is a source-breakpoint control below the production 700px minimum.
        window->setMinimumWidth(600); window->resize(width, 600); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId); QTRY_VERIFY(controller.canCreateChannel());
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("createChatTab-" + QString::number(tab)));
        click(window, visibleItem("createChatTab-" + QString::number(tab)));
        QTRY_COMPARE(visibleItem("createChatTabs")->height(), height);
        QCOMPARE(QQmlProperty::read(visibleItem("createChatTab-" + QString::number(tab)), "contentItem.font.pixelSize").toInt(), width < 640 ? 12 : 14);
        if (tab == 1) QCOMPARE(visibleItem("channelFormBody")->property("contentHeight").toReal(), qreal(600.53125));
        if (tab == 2) QTRY_COMPARE(visibleItem("globalTopicStreamSelector")->mapToScene(QPointF{}), QPointF(17, width == 700 ? 183 : 179));
        QCOMPARE(fixture.posts, 0); window->close();
    }

    void globalTopicSelectUsesSourceGeometryKeyboardAndVirtualOptions_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"en-light", "en-dark", "ru-light", "ru-dark", "empty", "virtual"}) QTest::newRow(name) << QString(name);
    }
    void globalTopicSelectUsesSourceGeometryKeyboardAndVirtualOptions()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini"));
        preferences.setLocale(scenario.startsWith("ru") ? "ru" : "en"); preferences.setMode(scenario.endsWith("dark") ? "dark" : "light");
        WindowTopicFixture fixture; WindowChannelFixture scene; scene.sourceScene = true;
        scene.users[0] = QJsonObject{{"uuid", userId}, {"username", "Cassi"}, {"email", "cassi@example.invalid"}};
        auto mainStream = fixture.streams[0].toObject(); mainStream["description"] = "Native preview";
        mainStream["unread_count"] = 3; mainStream["default_topic_uuid"] = topicId; fixture.streams[0] = mainStream;
        if (scenario == "empty") fixture.streams = {};
        if (scenario == "virtual") for (int index = 0; index < 500; ++index)
            fixture.streams.append(windowChannel(QString("87000000-0000-4000-8000-%1").arg(index, 12, 10, QLatin1Char('0')), QString("Team %1").arg(index)));
        const auto actual = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/actions/introspect") || path.endsWith("/users/") || path.endsWith("/messages/") || path.endsWith("/stream_topics/") && request.method == "GET" && fixture.created.isEmpty()) return scene.server.handler(request);
            return actual(request);
        };
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId); QTRY_VERIFY(controller.canCreateChannel());
        QTRY_VERIFY(controller.channelMembersVerified());
        if (scenario != "empty") { controller.selectStream(streamId); QTRY_COMPARE(controller.messages().size(), 7); QTRY_COMPARE(controller.topics().size(), 3); }
        openGlobalTopic(window);
        auto *selector = visibleItem("globalTopicStreamSelector"); auto *name = visibleItem("globalTopicNameInput");
        auto *dialog = window->findChild<QObject *>("createChatDialog"); QVERIFY(selector && name && dialog);
        const auto bounds = [](QQuickItem *target) { const auto point = target->mapToScene(QPointF{}); return QJsonObject{{"x",point.x()},{"y",point.y()},{"width",target->width()},{"height",target->height()}}; };
        QTRY_COMPARE(selector->mapToScene(QPointF{}), QPointF(289,333)); QCOMPARE(selector->size(), QSizeF(862,40));
        QTRY_COMPARE(name->mapToScene(QPointF{}), QPointF(289,417)); QCOMPARE(name->size(), QSizeF(862,40.84375));
        QCOMPARE(dialog->property("height").toReal(), qreal(512));
        QCOMPARE(QQmlProperty::read(selector,"background.radius").toReal(),qreal(6));
        selector->forceActiveFocus(Qt::MouseFocusReason); QTRY_VERIFY(selector->hasActiveFocus());
        QCOMPARE(QQmlProperty::read(selector,"background.border.color").value<QColor>(), QColor("#7087ff"));
        const auto captures = qEnvironmentVariable("CASSI_NATIVE_GLOBAL_TOPIC_SCREENSHOT_DIRECTORY");
        const auto capture = [&](const QString &state) {
            if (captures.isEmpty() || !(scenario.startsWith("en-") || scenario.startsWith("ru-"))) return;
            QSignalSpy ready(window, &QQuickWindow::frameSwapped); QTest::mouseMove(window,QPoint(50,50)); window->update(); QTRY_VERIFY(!ready.isEmpty());
            const auto base = QDir(captures).filePath("native-topic-global-v18-"+scenario+"-"+state); QVERIFY(window->grabWindow().save(base+".png"));
            QJsonObject metrics{{"synthetic",true},{"serviceAcceptance",false},{"constructor","QApplication"},{"dpr",window->devicePixelRatio()},
                {"renderer",int(window->rendererInterface()->graphicsApi())},{"selector",bounds(selector)},{"input",bounds(name)},
                {"dialog",QJsonObject{{"x",dialog->property("x").toReal()},{"y",dialog->property("y").toReal()},{"width",dialog->property("width").toReal()},{"height",dialog->property("height").toReal()}}},
                {"focused",selector->hasActiveFocus()},{"border",QQmlProperty::read(selector,"background.border.color").value<QColor>().name()}};
            if (state=="open") { auto *options=window->findChild<QObject *>("globalTopicOptions"); metrics["options"]=QJsonObject{{"x",options->property("x").toReal()},{"y",options->property("y").toReal()},{"width",options->property("width").toReal()},{"height",options->property("height").toReal()}}; }
            QFile output(base+"-metrics.json"); QVERIFY(output.open(QIODevice::WriteOnly)); output.write(QJsonDocument(metrics).toJson());
        };
        capture("closed");
        QTest::keyClick(window,Qt::Key_Down);
        auto *options=window->findChild<QObject *>("globalTopicOptions"); QTRY_VERIFY(options->property("opened").toBool());
        if (scenario=="empty") {
            QCOMPARE(options->property("height").toReal(),qreal(62)); QVERIFY(hasVisibleText(window->contentItem(),"No options available"));
            QVERIFY(!visibleItem("globalTopicCreateButton")->isEnabled()); window->close(); return;
        }
        if (scenario=="virtual") {
            QCOMPARE(controller.streams().size(),504); QVERIFY(itemCountWithPrefix(window->contentItem(),"globalTopicOption-")<30);
            QCOMPARE(options->property("height").toReal(),qreal(320)); window->close(); return;
        }
        QCOMPARE(options->property("x").toReal(),qreal(289)); QCOMPARE(options->property("y").toReal(),qreal(379));
        QCOMPARE(options->property("width").toReal(),qreal(862)); QCOMPARE(options->property("height").toReal(),qreal(154));
        capture("open");
        QTest::keyClick(window,Qt::Key_Escape); QTRY_VERIFY(!options->property("visible").toBool());
        QVERIFY(dialog->property("opened").toBool()); QVERIFY(selector->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Down); QTest::keyClick(window,Qt::Key_End); QTest::keyClick(window,Qt::Key_Return);
        QCOMPARE(window->property("globalTopicTarget").toString(),QString("88000000-0000-4000-8000-000000000003"));
        QTest::keyClick(window,Qt::Key_P); QTRY_VERIFY(options->property("opened").toBool());
        QTest::keyClick(window,Qt::Key_Return);
        QCOMPARE(window->property("globalTopicTarget").toString(),QString("88000000-0000-4000-8000-000000000001"));
        QTest::keyClick(window,Qt::Key_Tab); QTRY_VERIFY(name->hasActiveFocus());
        name->setProperty("text","Draft"); QTest::keyClick(window,Qt::Key_Return); QCOMPARE(fixture.posts,0);
        QTest::keyClick(window,Qt::Key_Escape); QTRY_VERIFY(!visibleItem("globalTopicNameInput")); window->close();
    }

    void globalTopicLifetimeDistinguishesNamesFromAccessChanges_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"rename","counter","role","delete","route","logout"}) QTest::newRow(name) << QString(name);
    }
    void globalTopicLifetimeDistinguishesNamesFromAccessChanges()
    {
        QFETCH(QString,scenario);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini")); WindowTopicFixture fixture;
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session); QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(),"synthetic","synthetic"); QTRY_COMPARE(session.projects().size(),1); session.selectProject(projectId); QTRY_VERIFY(controller.canCreateTopicIn(streamId));
        openGlobalTopic(window); click(window,visibleItem("globalTopicStreamSelector")); click(window,visibleItem("globalTopicOption-"+streamId));
        visibleItem("globalTopicNameInput")->setProperty("text","Draft");
        auto updated=fixture.streams[0].toObject();
        if (scenario=="logout") session.logout();
        else if (scenario=="route") controller.selectStream(streamId);
        else if (scenario=="delete") QVERIFY(controller.applyRealtimeEvent(QJsonObject{{"project_id",projectId},{"user_uuid",userId},{"uuid",returnedId},{"payload",QJsonObject{{"kind","stream.deleted"},{"uuid",streamId}}}}));
        else {
            if (scenario=="rename") updated["name"]="<b>Renamed</b>";
            if (scenario=="counter") { updated["unread_count"]=4; updated["active_unread_count"]=4; }
            if (scenario=="role") updated["role"]="member";
            updated["kind"]="stream.updated";
            QVERIFY(controller.applyRealtimeEvent(QJsonObject{{"project_id",projectId},{"user_uuid",userId},{"uuid",returnedId},{"payload",updated}}));
        }
        if (scenario=="rename" || scenario=="counter") {
            QVERIFY(visibleItem("globalTopicNameInput")); QCOMPARE(window->property("globalTopicTarget").toString(),streamId);
            QCOMPARE(visibleItem("globalTopicNameInput")->property("text").toString(),QString("Draft"));
        } else { QTRY_VERIFY(!visibleItem("globalTopicNameInput")); QCOMPARE(window->property("globalTopicTarget").toString(),QString()); }
        QCOMPARE(fixture.posts,0); window->close();
    }

    void globalTopicPreservesLocalStateAndDropdownBounds_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"draft","blur","viewport"}) QTest::newRow(name) << QString(name);
    }
    void globalTopicPreservesLocalStateAndDropdownBounds()
    {
        QFETCH(QString,scenario);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini")); WindowTopicFixture fixture;
        for (int i=0;i<25;++i) fixture.streams.append(windowChannel(QString("87000000-0000-4000-8000-%1").arg(i,12,10,QLatin1Char('0')),QString("Team %1").arg(i)));
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session); QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
            if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Window did not release focus before engine teardown",__FILE__,__LINE__);
        });
        if (scenario=="viewport") { window->setMinimumHeight(400); window->resize(700,400); }
        QVERIFY(QTest::qWaitForWindowExposed(window)); session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1); session.selectProject(projectId); QTRY_VERIFY(controller.canCreateTopicIn(streamId)); openGlobalTopic(window);
        auto *name=visibleItem("globalTopicNameInput"); auto *selector=visibleItem("globalTopicStreamSelector");
        if (scenario=="viewport") {
            QTRY_COMPARE(window->size(),QSize(700,400));
            // Visible becomes true before QQuickColumn positions the newly
            // selected panel. Measure only after its exact source layout.
            QTRY_COMPARE(selector->mapToScene(QPointF{}),QPointF(17,83));
        }
        if (scenario=="draft") {
            name->setProperty("text","Local topic draft"); const auto chosen=window->property("globalTopicTarget").toString();
            click(window,visibleItem("createChatTab-0")); click(window,visibleItem("createChatTab-2"));
            QCOMPARE(visibleItem("globalTopicNameInput")->property("text").toString(),QString("Local topic draft"));
            QCOMPARE(window->property("globalTopicTarget").toString(),chosen);
        } else {
            selector->forceActiveFocus(Qt::TabFocusReason); QTest::keyClick(window,Qt::Key_Down);
            auto *options=window->findChild<QObject *>("globalTopicOptions"); QTRY_VERIFY(options->property("opened").toBool());
            if (scenario=="blur") {
                name->forceActiveFocus(Qt::OtherFocusReason); QTRY_VERIFY(!options->property("visible").toBool());
                QSignalSpy selectorClicked(selector,SIGNAL(clicked())); QVERIFY(selectorClicked.isValid());
                // Map the single pointer input against a rendered layout.
                QSignalSpy rendered(window,&QQuickWindow::frameSwapped); window->update();
                QTRY_VERIFY(!rendered.isEmpty());
                QTest::mouseClick(window,Qt::LeftButton,{},selector->mapToScene(QPointF(selector->width()-21,selector->height()/2)).toPoint());
                QTRY_COMPARE(selectorClicked.size(),1);
                QVERIFY2(selector->hasActiveFocus(),"Clicking the decorative chevron must focus the source's single select button");
                QTRY_VERIFY(options->property("opened").toBool());
            }
            else {
                const auto point=selector->mapToScene(QPointF{}); const auto below=window->height()-point.y()-selector->height()-14;
                QVERIFY(below>point.y()-14 && below<320);
                QCOMPARE(options->property("height").toReal(),below);
                QCOMPARE(options->property("y").toReal(),point.y()+selector->height()+6);
                QVERIFY(options->property("y").toReal()+options->property("height").toReal()<=window->height()-8);
            }
        }
        QCOMPARE(fixture.posts,0); const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void globalTopicSelectorKeepsSourceChevronAspect_data()
    {
        QTest::addColumn<bool>("expanded"); QTest::newRow("closed") << false; QTest::newRow("open") << true;
    }
    void globalTopicSelectorKeepsSourceChevronAspect()
    {
        QFETCH(bool,expanded);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini")); preferences.setMode("light");
        WindowTopicFixture fixture; ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences); auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window)); session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1); session.selectProject(projectId); QTRY_VERIFY(controller.canCreateTopicIn(streamId)); openGlobalTopic(window);
        auto *selector=visibleItem("globalTopicStreamSelector"); selector->forceActiveFocus(Qt::TabFocusReason);
        if (expanded) { QTest::keyClick(window,Qt::Key_Down); QTRY_VERIFY(window->findChild<QObject *>("globalTopicOptions")->property("opened").toBool()); }
        QSignalSpy frame(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!frame.isEmpty());
        const auto image=window->grabWindow(); QVERIFY(!image.isNull()); const auto ratio=window->devicePixelRatio();
        const auto point=selector->mapToScene(QPointF{}); const auto right=point.x()+selector->width();
        const auto pixel=[&](int x,int y){return image.pixelColor(qRound(x*ratio),qRound(y*ratio));};
        const auto background=pixel(qRound(right-31),qRound(point.y()+11)); QRect ink;
        for(int y=qRound(point.y()+6);y<qRound(point.y()+35);++y) for(int x=qRound(right-33);x<qRound(right-8);++x) {
            const auto color=pixel(x,y);
            const auto distance=std::max({qAbs(color.red()-background.red()),qAbs(color.green()-background.green()),qAbs(color.blue()-background.blue())});
            const auto chroma=std::max({color.red(),color.green(),color.blue()})-std::min({color.red(),color.green(),color.blue()});
            if(distance>40 && chroma<45) ink=ink.isNull()?QRect(x,y,1,1):ink.united(QRect(x,y,1,1));
        }
        qInfo()<<"Chevron ink"<<ink<<"expected source 15x8 centered at"<<QPointF(right-21,point.y()+20);
        QVERIFY2(ink.height()>=7 && ink.height()<=9,"The 15x8 source SVG must keep its shallow aspect inside a 16px icon slot");
        QVERIFY(ink.width()>=14 && ink.width()<=16);
        QVERIFY(qAbs(ink.center().x()-(right-21))<=1);
        QVERIFY(qAbs(ink.center().y()-(point.y()+20))<=1);
        if (expanded) {
            auto *option=visibleItem("globalTopicOption-88000000-0000-4000-8000-000000000002"); QVERIFY(option);
            const auto row=option->mapToScene(QPointF{}); const auto rowRight=row.x()+option->width(); QRect check;
            for(int y=qRound(row.y()+4);y<qRound(row.y()+33);++y) for(int x=qRound(rowRight-31);x<qRound(rowRight-5);++x) {
                const auto color=pixel(x,y);
                if(color.blue()-color.red()>70 && color.blue()-color.green()>40 && color.red()<180)
                    check=check.isNull()?QRect(x,y,1,1):check.united(QRect(x,y,1,1));
            }
            qInfo()<<"Check ink"<<check<<"expected source 14x10";
            QVERIFY2(check.height()>=9 && check.height()<=12,"The source check SVG must preserve its non-square viewBox");
            QVERIFY(check.width()>=13 && check.width()<=15);
            QVERIFY(qAbs(check.center().x()-(rowRight-18))<=1);
            const auto graphics=window->rendererInterface()->graphicsApi();
            if(graphics!=QSGRendererInterface::Software && graphics!=QSGRendererInterface::Null && graphics!=QSGRendererInterface::Unknown) {
                const auto bottom=row.y()+149; // First row begins 5px inside a154px panel.
                const auto center=qRound(point.x()+selector->width()/2);
                const auto near=pixel(center,qRound(bottom+8)).red();
                const auto tail=pixel(center,qRound(bottom+27)).red();
                const auto far=pixel(center,qRound(bottom+42)).red();
                qInfo()<<"Dropdown shadow"<<near<<tail<<far<<"source 235/251/255";
                QVERIFY2(far-near>=12 && far-near<=30 && far-tail<=10,"The source shadow-xl must visibly fade below the open panel");
            } else {
                auto *surface=window->findChild<QQuickItem *>("globalTopicOptionsSurface"); QVERIFY(surface);
                QVERIFY(!surface->property("shadowsSupported").toBool());
                QVERIFY(!QQmlProperty::read(surface,"layer.enabled").toBool());
            }
        }
        window->close();
    }

    void globalNewChatStartsWithTheSourceDirectChatTab()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowChannelFixture fixture;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin().toString(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(controller.directChatDirectoryVerified());
        click(window, item(window->contentItem(), "createChatButton"));
        auto *tab = visibleItem("createChatTab-0");
        QVERIFY(tab);
        QVERIFY2(tab->isEnabled(), "Source Start chat is the enabled initial New chat tab");
        QTRY_VERIFY(visibleItem("directChatSearch"));
        QCOMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(), 0);
        QVERIFY(!visibleItem("channelNameInput"));
        QVERIFY(!visibleItem("channelCreateButton"));
        QCOMPARE(fixture.posts, 0);
    }

    void directChatRowsNavigateOnlyAfterVerifiedHttp_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"new", "existing", "applied503", "not-applied503", "encrypted", "archived", "403"})
            QTest::newRow(name) << QString(name);
    }
    void directChatRowsNavigateOnlyAfterVerifiedHttp()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowDirectFixture fixture;
        if (scenario == "existing" || scenario == "encrypted" || scenario == "archived") fixture.seed();
        if (scenario == "encrypted") { fixture.created["encryption"] = true; fixture.created["current_encryption_key_uuid"] = returnedId; }
        if (scenario == "archived") fixture.created["is_archived"] = true;
        if (scenario.endsWith("503")) fixture.postStatus = 503;
        if (scenario == "not-applied503") fixture.applyDirectPost = false;
        if (scenario == "403") fixture.lookupStatus = 403;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QSignalSpy opened(&controller, &NativeController::directChatOpened);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.canStartDirectChatWith(channelPeer));
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("direct-user-" + channelPeer));
        auto *peer = visibleItem("direct-user-" + channelPeer);
        peer->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, scenario == "existing" ? Qt::Key_Return : Qt::Key_Space);
        QTRY_VERIFY(controller.directChatBusy());
        QVERIFY(!visibleItem("direct-user-" + channelPeer)->isEnabled());
        click(window, visibleItem("createChatTab-1"));
        visibleItem("channelNameInput")->setProperty("text", "No parallel channel write");
        QVERIFY(!visibleItem("channelCreateButton")->isEnabled());
        click(window, visibleItem("createChatTab-0"));
        QTRY_VERIFY(!controller.directChatBusy());
        if (scenario == "encrypted" || scenario == "archived" || scenario == "403") {
            QCOMPARE(fixture.posts, 0); QCOMPARE(opened.size(), 0);
            QCOMPARE(controller.directChatPhase(), scenario == "403" ? QString("failed") : QString("unsupported"));
            QTRY_VERIFY(visibleItem("directChatError"));
            QCOMPARE(visibleItem("directChatError")->property("textFormat").toInt(), int(Qt::PlainText));
            QVERIFY(!visibleItem("directChatRetryButton"));
            QVERIFY(!visibleItem("directChatCheckButton"));
            window->close(); return;
        }
        if (scenario.endsWith("503")) {
            QTRY_VERIFY(controller.directChatUncertain());
            const auto attempt = controller.directChatForm().value("attemptId");
            QCOMPARE(fixture.posts, 1);
            QTRY_VERIFY(visibleItem("directChatCheckButton"));
            click(window, visibleItem("channelCancelButton"));
            QTRY_VERIFY(!visibleItem("directChatSearch"));
            click(window, item(window->contentItem(), "createChatButton"));
            QTRY_VERIFY(visibleItem("directChatCheckButton"));
            QCOMPARE(controller.directChatForm().value("attemptId"), attempt);
            QCOMPARE(fixture.posts, 1);
            click(window, visibleItem("directChatCheckButton"));
            QTRY_VERIFY(!controller.directChatBusy());
            QCOMPARE(fixture.posts, 1);
            if (scenario == "not-applied503") {
                QTRY_VERIFY(visibleItem("directChatRetryButton"));
                click(window, visibleItem("direct-user-" + channelPeer));
                QTRY_VERIFY(!controller.directChatBusy());
                QCOMPARE(fixture.posts, 1); // Same peer repeats GET-only Check.
                fixture.applyDirectPost = true; fixture.postStatus = 201;
                click(window, visibleItem("directChatRetryButton"));
                QTRY_VERIFY(!controller.directChatBusy());
                QCOMPARE(fixture.posts, 2);
                QCOMPARE(fixture.directBodies[0], fixture.directBodies[1]);
            }
            QCOMPARE(controller.directChatForm().value("attemptId"), attempt);
        }
        QTRY_COMPARE(opened.size(), 1);
        QTRY_VERIFY(!visibleItem("directChatSearch"));
        QCOMPARE(controller.selectedStream(), windowDirectId());
        QCOMPARE(controller.selectedTopic(), QString{});
        QCOMPARE(controller.conversationKind(), QString("stream"));
        QTRY_VERIFY(visibleItem("streamTopicPrompt"));
        QVERIFY(!controller.canSend() && !controller.canAttach());
        QTRY_VERIFY(visibleItem("directConversationName"));
        QCOMPARE(visibleItem("directConversationName")->property("text").toString(), QString("Alice"));
        QTRY_COMPARE(visibleItem("conversationHeader")->height(), qreal(56));
        QCOMPARE(item(window->contentItem(), "stream-" + windowDirectId())->property("enabled").toBool(), true);
        QCOMPARE(fixture.memberPosts, 0);
        QCOMPARE(fixture.posts, scenario == "existing" ? 0 : scenario == "not-applied503" ? 2 : 1);
        if (!fixture.directBodies.isEmpty())
            QCOMPARE(fixture.directBodies.first(), QJsonObject({{"name", "Direct"}, {"description", "Private workspace"},
                {"source_name", "native"}, {"direct_user_uuid", channelPeer}}));
        click(window, item(window->contentItem(), "topic-" + channelDefault));
        QTRY_COMPARE(controller.selectedTopic(), channelDefault);
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(visibleItem("directConversationName")->property("text").toString(), QString("Alice"));
        controller.setDraft("A real selected topic enables the composer");
        QVERIFY(controller.canSend());
        window->close();
    }

    void directChatRecoveryRemainsInTheKeyboardFocusLoop()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowDirectFixture fixture; fixture.postStatus = 503; fixture.applyDirectPost = false;
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.canStartDirectChatWith(channelPeer));
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("direct-user-" + channelPeer)); click(window, visibleItem("direct-user-" + channelPeer));
        QTRY_VERIFY(controller.directChatUncertain());
        QTRY_VERIFY(visibleItem("directChatCheckButton"));
        auto *cancel = visibleItem("channelCancelButton");
        cancel->forceActiveFocus(Qt::TabFocusReason); QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY2(visibleItem("directChatCheckButton")->hasActiveFocus(),
            "Keyboard users must reach GET-only Check status from Cancel");
        QTest::keyClick(window, Qt::Key_Space); QTRY_VERIFY(!controller.directChatBusy());
        QCOMPARE(fixture.posts, 1);
        cancel->forceActiveFocus(Qt::TabFocusReason); QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(visibleItem("directChatCheckButton")->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(visibleItem("directChatRetryButton")->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(visibleItem("createChatClose")->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Backtab);
        QTRY_VERIFY(visibleItem("directChatRetryButton")->hasActiveFocus());
        QCOMPARE(fixture.posts, 1); window->close();
    }

    void directAndChannelRecoveryRestoresTheLastViewedUnresolvedTab()
    {
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowDirectFixture fixture; fixture.postStatus = 503; fixture.applyDirectPost = false; fixture.postDelay = 400;
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.canStartDirectChatWith(channelPeer));
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("direct-user-" + channelPeer)); click(window, visibleItem("direct-user-" + channelPeer));
        QTRY_VERIFY(controller.directChatUncertain());
        const auto directAttempt = controller.directChatForm().value("attemptId");
        click(window, visibleItem("createChatTab-1"));
        visibleItem("channelNameInput")->setProperty("text", "An independent unresolved channel");
        click(window, visibleItem("channelCreateButton"));
        QTRY_VERIFY(controller.channelCreationBusy());
        click(window, visibleItem("createChatTab-0"));
        QVERIFY(!visibleItem("direct-user-" + channelPeer)->isEnabled());
        click(window, visibleItem("createChatTab-1"));
        QTRY_VERIFY(controller.channelCreationUncertain());
        const auto channelAttempt = controller.channelCreationForm().value("attemptId");
        QVERIFY(controller.directChatUncertain());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!visibleItem("channelNameInput"));
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("channelCheckButton"));
        QCOMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(), 1);
        QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString("An independent unresolved channel"));
        click(window, visibleItem("createChatTab-0"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!visibleItem("directChatSearch"));
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("directChatCheckButton"));
        QCOMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(), 0);
        QCOMPARE(controller.directChatForm().value("attemptId"), directAttempt);
        QCOMPARE(controller.channelCreationForm().value("attemptId"), channelAttempt);
        QCOMPARE(fixture.posts, 2); window->close();
    }

    void directChatRosterUsesSourceGeometrySearchAndVirtualRows_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"en-light", "en-dark", "ru-light", "ru-dark", "narrow", "virtual", "empty"})
            QTest::newRow(name) << QString(name);
    }
    void directChatRosterUsesSourceGeometrySearchAndVirtualRows()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("appearance.ini"));
        preferences.setLocale(scenario.startsWith("ru") ? "ru" : "en");
        preferences.setMode(scenario.endsWith("dark") ? "dark" : "light");
        WindowDirectFixture fixture;
        fixture.sourceScene = scenario.startsWith("en-") || scenario.startsWith("ru-");
        if (fixture.sourceScene) fixture.users[0] = QJsonObject{{"uuid", userId}, {"username", "Cassi"}, {"email", "cassi@example.invalid"}};
        fixture.users[1] = QJsonObject{{"uuid", channelPeer}, {"username", "hidden-handle"},
            {"first_name", "Alice"}, {"email", "alice@example.invalid"}};
        fixture.users.append(QJsonObject{{"uuid", returnedId}, {"username", "bob"}, {"first_name", "Bob"}});
        if (scenario == "empty") fixture.users = QJsonArray{fixture.users.first()};
        if (scenario == "virtual") for (int index = 0; index < 500; ++index)
            fixture.users.append(QJsonObject{{"uuid", QString("86000000-0000-4000-8000-%1").arg(index, 12, 10, QLatin1Char('0'))},
                {"username", QString("person%1").arg(index)}, {"first_name", QString("Person %1").arg(index)}});
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
            if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Roster window did not release focus before teardown",__FILE__,__LINE__);
        });
        if (scenario == "narrow") window->resize(700, 600);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.directChatDirectoryVerified());
        if (fixture.sourceScene) {
            controller.selectStream(streamId);
            QTRY_COMPARE(controller.messages().size(), 7);
            QTRY_COMPARE(controller.topics().size(), 3);
        }
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("directChatSearch"));
        auto *search = visibleItem("directChatSearch");
        auto *roster = visibleItem("directChatRoster");
        auto *dialog = engine.rootObjects().first()->findChild<QObject *>("createChatDialog");
        QVERIFY(dialog && roster);
        QCOMPARE(dialog->property("height").toReal(), qreal(512));
        QCOMPARE(roster->height(), qreal(240));
        QCOMPARE(search->height(), qreal(40.84375));
        QTRY_COMPARE(search->mapToScene(QPointF{}), scenario == "narrow" ? QPointF(17, 151) : QPointF(289, 301));
        QVERIFY(!visibleItem("direct-user-" + userId));
        QVERIFY(!visibleItem("channelCreateButton"));
        QVERIFY(!visibleItem("channelEncryption"));
        auto *tab = visibleItem("createChatTab-0"); tab->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_End);
        QTRY_COMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(), 3);
        QTest::keyClick(window, Qt::Key_Home);
        QTRY_COMPARE(visibleItem("createChatTabs")->property("currentIndex").toInt(), 0);
        if (scenario == "empty") { QVERIFY(hasVisibleText(window->contentItem(), "No results")); const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed); return; }
        if (scenario == "virtual") {
            QCOMPARE(controller.directChatChoices().size(), 502);
            QTRY_VERIFY(itemCountWithPrefix(window->contentItem(), "direct-user-") > 0);
            QVERIFY(itemCountWithPrefix(window->contentItem(), "direct-user-") < 30);
        }
        search->setProperty("text", " HIDDEN-HANDLE ");
        QTRY_VERIFY(visibleItem("direct-user-" + channelPeer));
        QVERIFY(!visibleItem("direct-user-" + returnedId));
        QCOMPARE(visibleItem("direct-name-" + channelPeer)->property("textFormat").toInt(), int(Qt::PlainText));
        click(window, visibleItem("createChatTab-1"));
        QCOMPARE(visibleItem("channelMemberSearch")->property("text").toString(), QString(" HIDDEN-HANDLE "));
        click(window, visibleItem("createChatTab-0"));
        search->setProperty("text", "No synthetic result");
        QTRY_VERIFY(hasVisibleText(window->contentItem(), scenario.startsWith("ru") ? QString("Ничего не найдено") : QString("No results")));
        QCOMPARE(roster->height(), qreal(240)); QCOMPARE(dialog->property("height").toReal(), qreal(512));
        search->setProperty("text", "");
        QTRY_VERIFY(visibleItem("direct-user-" + channelPeer));
        search->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(search->hasActiveFocus());
        QCOMPARE(QQmlProperty::read(search->property("background").value<QQuickItem *>(), "border.color").value<QColor>(), QColor("#7087ff"));
        QTest::keyClick(window, Qt::Key_Return); QCOMPARE(fixture.posts, 0);
        const auto captures = qEnvironmentVariable("CASSI_NATIVE_DIRECT_SCREENSHOT_DIRECTORY");
        if (!captures.isEmpty() && (scenario.startsWith("en-") || scenario.startsWith("ru-"))) {
            QSignalSpy ready(window, &QQuickWindow::frameSwapped);
            QTest::mouseMove(window, QPoint(50, 50)); window->update(); QTRY_VERIFY(!ready.isEmpty());
            const auto base = QDir(captures).filePath("native-direct-v17-" + scenario);
            QVERIFY(window->grabWindow().save(base + ".png"));
            const auto bounds = [](QQuickItem *target) {
                const auto point = target->mapToScene(QPointF{});
                return QJsonObject{{"x", point.x()}, {"y", point.y()}, {"width", target->width()}, {"height", target->height()}};
            };
            QJsonObject metrics{{"synthetic", true}, {"serviceAcceptance", false}, {"constructor", "QApplication"},
                {"locale", preferences.locale()}, {"mode", preferences.mode()}, {"dpr", window->devicePixelRatio()},
                {"dialog", QJsonObject{{"x", dialog->property("x").toReal()}, {"y", dialog->property("y").toReal()},
                    {"width", dialog->property("width").toReal()}, {"height", dialog->property("height").toReal()}}},
                {"search", bounds(search)}, {"roster", bounds(roster)}, {"cancel", bounds(visibleItem("channelCancelButton"))},
                {"searchFocused", search->hasActiveFocus()}, {"searchBorder", QQmlProperty::read(search->property("background").value<QQuickItem *>(), "border.color").value<QColor>().name()}};
            QFile output(base + "-metrics.json"); QVERIFY(output.open(QIODevice::WriteOnly)); output.write(QJsonDocument(metrics).toJson());
        }
        QCOMPARE(fixture.posts, 0);
        const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void directChatViewKeepsOnlyItsOwnAttemptAndContext_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"dismiss-pending", "logout", "route", "benign-metadata", "directory-unavailable"})
            QTest::newRow(name) << QString(name);
    }
    void directChatViewKeepsOnlyItsOwnAttemptAndContext()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini"));
        WindowDirectFixture fixture;
        fixture.canonicalDelay = 150; fixture.postDelay = 150;
        if (scenario == "directory-unavailable") {
            const auto fallback = fixture.server.handler;
            fixture.server.handler = [fallback](const Request &request) {
                return request.target.path().endsWith("/users/") ? Response{QJsonObject{}, 503} : fallback(request);
            };
        }
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QSignalSpy opened(&controller, &NativeController::directChatOpened);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateChannel());
        click(window, item(window->contentItem(), "createChatButton"));
        QTRY_VERIFY(visibleItem("directChatSearch"));
        auto *rootObject = engine.rootObjects().first();
        if (scenario == "directory-unavailable") {
            QTRY_VERIFY(hasVisibleText(window->contentItem(), "User list is currently unavailable."));
            QVERIFY(!visibleItem("direct-user-" + channelPeer));
            QVERIFY(!visibleItem("directChatSearch")->isEnabled());
            QCOMPARE(fixture.posts, 0); window->close(); return;
        }
        QTRY_VERIFY(controller.canStartDirectChatWith(channelPeer));
        if (scenario == "benign-metadata") {
            const auto context = rootObject->property("directShellContext");
            visibleItem("directChatSearch")->setProperty("text", "alice");
            auto payload = QJsonObject{{"kind", "user.updated"}, {"uuid", channelPeer}, {"username", "alice"},
                {"first_name", "<b>Alice updated</b>"}, {"email", "new@example.invalid"}};
            QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
                {"uuid", "87000000-0000-4000-8000-000000000001"}, {"payload", payload}}));
            QTRY_VERIFY(visibleItem("direct-name-" + channelPeer));
            QCOMPARE(rootObject->property("directShellContext"), context);
            QCOMPARE(visibleItem("direct-name-" + channelPeer)->property("text").toString(), QString("<b>Alice updated</b>"));
            QCOMPARE(visibleItem("direct-name-" + channelPeer)->property("textFormat").toInt(), int(Qt::PlainText));
            QCOMPARE(visibleItem("directChatSearch")->property("text").toString(), QString("alice"));
            click(window, visibleItem("createChatTab-1"));
            visibleItem("channelNameInput")->setProperty("text", "Preserved while visiting Start chat");
            click(window, visibleItem("channel-member-" + channelPeer));
            click(window, visibleItem("createChatTab-0"));
            click(window, visibleItem("createChatTab-1"));
            QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString("Preserved while visiting Start chat"));
            QCOMPARE(rootObject->property("channelSelectedMembers").toList(), QVariantList{channelPeer});
            QCOMPARE(fixture.posts, 0); window->close(); return;
        }
        click(window, visibleItem("direct-user-" + channelPeer));
        QTRY_VERIFY(controller.directChatBusy());
        const auto attempt = controller.directChatForm().value("attemptId");
        if (scenario == "logout") controller.logout();
        else if (scenario == "route") controller.selectStream(streamId);
        else {
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!visibleItem("directChatSearch"));
            QCOMPARE(controller.directChatForm().value("attemptId"), attempt);
            click(window, item(window->contentItem(), "createChatButton"));
            QTRY_VERIFY(visibleItem("directChatStatus"));
            QCOMPARE(rootObject->property("directViewAttempt"), attempt);
            QCOMPARE(rootObject->property("directViewPeer").toString(), channelPeer);
        }
        if (scenario != "dismiss-pending") {
            QTRY_VERIFY(!visibleItem("directChatSearch"));
            QCOMPARE(rootObject->property("directViewPeer").toString(), QString{});
            QCOMPARE(rootObject->property("chatUserQuery").toString(), QString{});
        }
        QTRY_VERIFY(!controller.directChatBusy());
        if (scenario == "dismiss-pending") {
            QTRY_COMPARE(opened.size(), 1);
            QTRY_VERIFY(!visibleItem("directChatSearch"));
            QCOMPARE(fixture.posts, 1);
        } else {
            QCOMPARE(opened.size(), 0);
            QVERIFY(controller.selectedStream() != windowDirectId());
        }
        window->close();
    }

    void savedHistorySurvivesProcessRestartAndRevalidatesBeforeActions()
    {
        QTemporaryDir directory;
        MockServer server;
        QString phase, activeUser = userId;
        int historyGets = 0, aggregateGets = 0, mutations = 0;
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.contains("/messenger/") && request.method != "GET") {
                ++mutations;
                return Response{QJsonObject{}, 500};
            }
            if (path.endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Saved conversation"}, {"encryption", false}}}, 200, phase == "restore" ? 350 : 0};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/"))
                return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "History"}}}, 200, phase == "restore" ? 350 : 0};
            if (path.endsWith("/messages/")) {
                const bool aggregate = !QUrlQuery(request.target).hasQueryItem("topic_uuid");
                if (aggregate) ++aggregateGets;
                else ++historyGets;
                auto current = message(phase == "seed" ? "Saved message before restart" : "Verified updated message after reconnect");
                current["read"] = phase != "seed";
                current["reactions"] = QJsonObject{{"heart", 1}};
                current["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
                if (phase == "seed") {
                    auto deleted = message("This saved row is deleted before reconnect", returnedId);
                    deleted["read"] = false;
                    return Response{QJsonArray{current, deleted}};
                }
                if (!aggregate && historyGets == 1) return Response{QJsonObject{}, 503, 900};
                return Response{QJsonArray{current}, 200, 700};
            }
            if (path.endsWith("/actions/get_token/invoke") && request.body.value("grant_type") == "login+password")
                activeUser = request.body.value("login").toString() == "second-synthetic-user"
                    ? "88888888-8888-4888-8888-888888888888" : userId;
            auto response = sessionResponse(request);
            if (path.endsWith("/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", QJsonObject{{"uuid", activeUser}}}};
            return response;
        };
        for (const auto &run : {QString("seed"), QString("restore")}) {
            phase = run;
            historyGets = 0;
            aggregateGets = 0;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("CASSI_NATIVE_CACHE_CHILD_PHASE", phase);
            environment.insert("CASSI_NATIVE_CACHE_CHILD_DIRECTORY", directory.path());
            environment.insert("CASSI_NATIVE_CACHE_CHILD_ORIGIN", server.origin().toString());
            QProcess process;
            process.setProcessEnvironment(environment);
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start(QCoreApplication::applicationFilePath(), {"cachePhase"});
            QVERIFY(process.waitForStarted());
            QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 20000);
            const auto output = process.readAll();
            const auto logDirectory = qEnvironmentVariable("CASSI_NATIVE_TEST_CACHE_LOG_DIRECTORY");
            if (!logDirectory.isEmpty()) {
                QFile log(QDir(logDirectory).filePath("native-cache-process-" + phase + ".log"));
                QVERIFY(log.open(QIODevice::WriteOnly | QIODevice::Truncate));
                QCOMPARE(log.write(output), output.size());
            }
            QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, output.constData());
            QCOMPARE(historyGets, phase == "seed" ? 1 : 3);
            QCOMPARE(aggregateGets, 1);
            QCOMPARE(mutations, 0);
            qInfo("Cache process phase %s: topic-history GETs=%d, aggregate-history GETs=%d, messenger mutations=%d", qPrintable(phase), historyGets, aggregateGets, mutations);
        }
    }

    void warmNavigationUsesVerifiedSavedPagesWithoutExtraHttpRequests()
    {
        QTemporaryDir directory;
        NativeCacheService cache(directory.path(), {}, true);
        NativePreferences preferences(directory.filePath("appearance.ini"));
        MockServer server;
        int historyGets = 0, aggregateGets = 0, topicGets = 0;
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Warm cache"}, {"encryption", false}}}};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) {
                ++topicGets;
                QJsonArray topics;
                for (const auto &id : {topicId, secondTopicId})
                    topics.append(QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", id == topicId ? "First" : "Second"}});
                return Response{topics};
            }
            if (path.endsWith("/messages/")) {
                const auto id = QUrlQuery(request.target).queryItemValue("topic_uuid");
                if (id.isEmpty()) {
                    ++aggregateGets;
                    return Response{QJsonArray{message("First verified page", messageId, topicId),
                        message("Second verified page", returnedId, secondTopicId)}};
                }
                ++historyGets;
                return Response{QJsonArray{message(id == topicId ? "First verified page" : "Second verified page",
                    id == topicId ? messageId : returnedId, id)}};
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.useCache(&cache);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *content = window->contentItem();
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        click(window, item(content, "project-" + projectId));
        QTRY_COMPARE(controller.streams().size(), 1);
        click(window, item(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 2);
        for (const auto &id : {topicId, secondTopicId, topicId}) {
            click(window, item(content, "topic-" + id));
            QTRY_VERIFY(!controller.busy());
            QTRY_VERIFY(item(content, "message-content-" + (id == topicId ? messageId : returnedId)));
            QVERIFY(!controller.cacheStale());
        }
        QCOMPARE(historyGets, 2);
        QCOMPARE(aggregateGets, 1);
        QCOMPARE(topicGets, 1);
        click(window, item(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 2);
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.messages().size(), 2);
        click(window, item(content, "topic-" + secondTopicId));
        QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(item(content, "message-content-" + returnedId));
        QCOMPARE(historyGets, 2);
        QCOMPARE(aggregateGets, 1);
        QCOMPARE(topicGets, 1);
        QTRY_VERIFY(cache.stats().hits >= 3);
        qInfo("Warm navigation: 4 topic selections, topic-history GETs=%d; 2 stream selections, aggregate-history GETs=%d, topic-list GETs=%d; cache hits=%llu",
            historyGets, aggregateGets, topicGets, static_cast<unsigned long long>(cache.stats().hits));
        window->close();
    }

    void globalNewChatOffersOrdinaryChannelCreation()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        preferences.setMode("light");
        MockServer server;
        int mutations = 0;
        server.handler = [&](const Request &request) {
            if (request.target.path().contains("/messenger/") && request.method != "GET")
                ++mutations;
            if (request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Product team"}, {"encryption", false}}}};
            if (request.target.path().endsWith("/users/")) return Response{QJsonArray{}};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        QTRY_VERIFY(controller.canCreateChannel());
        auto *entry = item(window->contentItem(), "createChatButton");
        QVERIFY(entry);
        QVERIFY2(entry->isEnabled(), "The source New chat entry must open the ordinary channel creation shell");
        openChannelNewChat(window);
        QTRY_VERIFY(visibleItem("channelNameInput"));
        auto *name = visibleItem("channelNameInput");
        auto *description = visibleItem("channelDescriptionInput");
        auto *submit = visibleItem("channelCreateButton");
        auto *close = visibleItem("createChatClose");
        auto *dialog = engine.rootObjects().first()->findChild<QObject *>("createChatDialog");
        QVERIFY(name && description && submit && close && dialog);
        auto *dialogContent = visibleItem("createChatContent");
        QVERIFY(dialogContent);
        auto *accessible = QAccessible::queryAccessibleInterface(dialogContent);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Dialog);
        QCOMPARE(accessible->text(QAccessible::Name), QString("New chat"));
        QTRY_VERIFY(visibleItem("createChatTab-1")->hasActiveFocus());
        QVERIFY(!visibleItem("createChatTab-1")->property("visualFocus").toBool());
        QCOMPARE(dialog->property("width").toReal(), qreal(896));
        QCOMPARE(dialog->property("height").toReal(), qreal(749.53125));
        auto *surface = visibleItem("createChatSurface");
        QVERIFY(surface);
        if (window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
            QVERIFY(!surface->property("shadowsSupported").toBool());
            QVERIFY(!QQmlProperty::read(surface, "layer.enabled").toBool());
        }
        auto *channelTab = dialogButton("Create channel");
        QVERIFY(channelTab);
        QCOMPARE(channelTab->height(), qreal(38));
        auto *tabs = visibleItem("createChatTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->property("count").toInt(), 4);
        const QStringList labels{"Start chat", "Create channel", "Create topic", "Archive"};
        for (int index = 0; index < labels.size(); ++index) {
            auto *tab = visibleItem("createChatTab-" + QString::number(index));
            QVERIFY(tab);
            QCOMPARE(tab->property("text").toString(), labels[index]);
            QCOMPARE(tab->isEnabled(), index <= 3);
        }
        QCOMPARE(channelTab->mapToItem(tabs, QPointF(0, 0)).y(), qreal(0));
        QCOMPARE(channelTab->mapToItem(tabs, QPointF(0, 0)).x(), tabs->width() / 4);
        QCOMPARE(name->width(), qreal(862));
        QCOMPARE(name->height(), qreal(40.84375));
        QCOMPARE(description->height(), qreal(40.84375));
        QVERIFY(!submit->isEnabled());
        name->setProperty("text", "   ");
        QVERIFY(!submit->isEnabled());
        name->setProperty("text", "Native planning");
        QVERIFY(submit->isEnabled());
        name->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Enter);
        QCOMPARE(mutations, 0);
        QVERIFY(visibleItem("channelNameInput"));
        auto *invite = visibleItem("channelInviteOnly");
        auto *announce = visibleItem("channelAnnounce");
        QVERIFY(invite && announce);
        QVERIFY(!invite->property("checked").toBool());
        QVERIFY(!announce->property("checked").toBool());
        click(window, invite);
        QVERIFY(invite->property("checked").toBool());
        auto *indicator = invite->property("indicator").value<QQuickItem *>();
        QVERIFY(indicator);
        const auto whiteTickHeight = [&] {
            const auto pixels = window->grabWindow();
            const auto origin = indicator->mapToScene(QPointF(0, 0));
            int first = 16, last = -1;
            for (int y = 2; y < 14; ++y)
                for (int x = 2; x < 14; ++x) {
                    const auto pixel = pixels.pixelColor(qRound((origin.x() + x) * window->devicePixelRatio()),
                        qRound((origin.y() + y) * window->devicePixelRatio()));
                    if (pixel.red() >= 220 && pixel.green() >= 220 && pixel.blue() >= 220) {
                        first = qMin(first, y); last = qMax(last, y);
                    }
                }
            return last < first ? 0 : last - first + 1;
        };
        // Original source PNG: 10×9px white tick; allow one raster edge pixel.
        QTRY_VERIFY2(whiteTickHeight() >= 8 && whiteTickHeight() <= 10,
            qPrintable(QString("White tick height %1, source expected 8–10 raster pixels").arg(whiteTickHeight())));
        QCOMPARE(indicator->property("color").value<QColor>(), QColor("#0075ff"));
        QVERIFY(!announce->property("checked").toBool());
        click(window, announce);
        QVERIFY(announce->property("checked").toBool());
        QVERIFY(!visibleItem("channelAnnouncementOnly")->isEnabled());
        QVERIFY(!visibleItem("channelEncryption")->isEnabled());
        if (!qEnvironmentVariableIsEmpty("CASSI_NATIVE_CHANNEL_SHELL_SCREENSHOT")) {
            QTest::mouseMove(window, QPoint(50, 50));
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("CASSI_NATIVE_CHANNEL_SHELL_SCREENSHOT")));
        }
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!visibleItem("channelNameInput"));
        QTRY_VERIFY(entry->hasActiveFocus());
        auto *previous = entry->nextItemInFocusChain(false);
        QVERIFY(previous && previous != entry);
        previous->forceActiveFocus(Qt::MouseFocusReason);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(entry->hasActiveFocus());
        QVERIFY(entry->property("visualFocus").toBool());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(visibleItem("directChatSearch"));
        QTRY_VERIFY(visibleItem("createChatClose")->hasActiveFocus());
        QVERIFY(visibleItem("createChatClose")->property("visualFocus").toBool());
        click(window, visibleItem("createChatTab-1"));
        QTRY_VERIFY(visibleItem("channelNameInput"));
        QTRY_VERIFY(visibleItem("createChatTab-1")->hasActiveFocus());
        QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString());
        QVERIFY(!visibleItem("channelInviteOnly")->property("checked").toBool());
        QVERIFY(!visibleItem("channelAnnounce")->property("checked").toBool());
        session.logout();
        QTRY_VERIFY(!visibleItem("channelNameInput"));
        QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList().size(), 0);
        QCOMPARE(mutations, 0);
        QCOMPARE(controller.selectedStream(), QString());
        QCOMPARE(controller.selectedTopic(), QString());
        window->close();
    }

    void globalChannelCreationUsesVerifiedHttp_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"empty", "selected", "filter", "virtual", "en-light", "en-dark", "ru-light", "ru-dark"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void globalChannelCreationUsesVerifiedHttp()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale(scenario.startsWith("ru-") ? "ru" : "en");
        preferences.setMode(scenario.endsWith("dark") ? "dark" : "light");
        WindowChannelFixture fixture;
        fixture.sourceScene = scenario.startsWith("en-") || scenario.startsWith("ru-");
        if (fixture.sourceScene) {
            fixture.users[0] = QJsonObject{{"uuid", userId}, {"username", "Cassi"}, {"email", "cassi@example.invalid"}};
            fixture.users[1] = QJsonObject{{"uuid", channelPeer}, {"username", "Alice"}, {"email", "alice@example.invalid"}};
        }
        if (scenario == "filter")
            fixture.users.append(QJsonObject{{"uuid", returnedId}, {"username", "bob"},
                {"first_name", "Bob"}, {"email", "bob@example.invalid"}});
        if (scenario == "virtual")
            for (int number = 0; number < 500; ++number)
                fixture.users.append(QJsonObject{{"uuid", QString("83000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'))},
                    {"username", QString("member%1").arg(number)}, {"first_name", QString("Member %1").arg(number)},
                    {"email", QString("member%1@example.invalid").arg(number)}});
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateChannel());
        QTRY_VERIFY(controller.channelMembersVerified());
        const auto initialUsers = fixture.usersGets;
        const auto initialCatalogs = fixture.catalogGets;
        if (fixture.sourceScene) {
            controller.selectStream(streamId);
            QTRY_COMPARE(controller.messages().size(), 7);
            QTRY_COMPARE(controller.topics().size(), 3);
        }
        const auto initialHistory = fixture.historyGets;
        auto *content = window->contentItem();
        openChannelNewChat(window);
        QTRY_VERIFY(visibleItem("channelNameInput"));
        if (fixture.sourceScene) {
            auto *archiveTab = visibleItem("createChatTab-3");
            QVERIFY(archiveTab);
            QCOMPARE(archiveTab->property("text").toString(), scenario.startsWith("ru-") ? QString("Архив") : QString("Archive"));
        }
        auto *name = visibleItem("channelNameInput");
        auto *description = visibleItem("channelDescriptionInput");
        auto *search = visibleItem("channelMemberSearch");
        QVERIFY(name && description && search);
        QCOMPARE(fixture.posts, 0);
        QCOMPARE(fixture.memberPosts, 0);
        QVERIFY(!visibleItem("channel-member-" + userId));
        if (scenario != "empty") {
            QTRY_VERIFY(visibleItem("channel-member-" + channelPeer));
            click(window, visibleItem("channel-member-" + channelPeer));
            QTRY_VERIFY(visibleItem("channel-member-" + channelPeer)->property("checked").toBool());
        }
        if (scenario == "filter") {
            search->setProperty("text", "  BOB@EXAMPLE.INVALID  ");
            QTRY_VERIFY(visibleItem("channel-member-" + returnedId));
            QVERIFY(!visibleItem("channel-member-" + channelPeer));
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList(), QVariantList{channelPeer});
            search->setProperty("text", "does-not-exist");
            QTRY_VERIFY(hasVisibleText(content, "No results"));
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList(), QVariantList{channelPeer});
            search->setProperty("text", "alice");
            QTRY_VERIFY(visibleItem("channel-member-" + channelPeer));
            QVERIFY(visibleItem("channel-member-" + channelPeer)->property("checked").toBool());
            search->setProperty("text", "");
        }
        if (scenario == "virtual") {
            QCOMPARE(controller.channelMemberChoices().size(), 501);
            QTRY_VERIFY(itemCountWithPrefix(content, "channel-member-") > 0);
            QVERIFY(itemCountWithPrefix(content, "channel-member-") < 30);
            search->setProperty("text", "member499@example.invalid");
            QTRY_VERIFY(visibleItem("channel-member-83000000-0000-4000-8000-000000000499"));
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList(), QVariantList{channelPeer});
            search->setProperty("text", "");
        }
        name->setProperty("text", fixture.sourceScene ? "Native planning" : "  Native planning  ");
        description->setProperty("text", fixture.sourceScene ? "Ordinary channel reference" : "  Ordinary channel reference  ");
        click(window, visibleItem("channelInviteOnly"));
        click(window, visibleItem("channelAnnounce"));
        auto *submit = visibleItem("channelCreateButton");
        QVERIFY(submit && submit->isEnabled());
        const auto screenshotDirectory = qEnvironmentVariable("CASSI_NATIVE_CHANNEL_SCREENSHOT_DIRECTORY");
        if (fixture.sourceScene && !screenshotDirectory.isEmpty()) {
            // The pinned source scene ends its search/filter check with this
            // field focused. Capture the same interaction state.
            QSignalSpy frameReady(window, &QQuickWindow::frameSwapped);
            search->forceActiveFocus(Qt::MouseFocusReason);
            QTRY_VERIFY(search->hasActiveFocus());
            QTRY_VERIFY(!frameReady.isEmpty());
            QTest::mouseMove(window, QPoint(50, 50));
            const QString base = QDir(screenshotDirectory).filePath("native-channel-v16-" + scenario + "-members");
            QVERIFY(window->grabWindow().save(base + ".png"));
            auto *dialog = engine.rootObjects().first()->findChild<QObject *>("createChatDialog");
            QVERIFY(dialog);
            const auto bounds = [](QQuickItem *target) {
                const auto point = target->mapToScene(QPointF(0, 0));
                return QJsonObject{{"x", point.x()}, {"y", point.y()}, {"width", target->width()}, {"height", target->height()}};
            };
            const QJsonObject metrics{{"synthetic", true}, {"serviceAcceptance", false}, {"dateFixtureSeam", false},
                {"constructor", "QApplication"}, {"locale", preferences.locale()}, {"mode", preferences.mode()},
                {"width", window->width()}, {"height", window->height()}, {"dpr", window->devicePixelRatio()},
                {"font", QFontInfo(name->property("font").value<QFont>()).family()},
                {"graphicsApi", int(window->rendererInterface()->graphicsApi())},
                {"shadowSupported", engine.rootObjects().first()->findChild<QObject *>("createChatSurface")->property("shadowsSupported").toBool()},
                {"baseline", "95495b10b828f853c9e770b47216073eb973fa95"},
                {"card", QJsonObject{{"x", dialog->property("x").toReal()}, {"y", dialog->property("y").toReal()},
                    {"width", dialog->property("width").toReal()}, {"height", dialog->property("height").toReal()}}},
                {"name", bounds(name)}, {"description", bounds(description)},
                {"body", bounds(visibleItem("channelFormBody"))},
                {"tabs", bounds(visibleItem("createChatTabs"))},
                {"search", bounds(search)},
                {"members", bounds(visibleItem("channelMemberList")->parentItem())},
                {"memberList", bounds(visibleItem("channelMemberList"))},
                {"footer", bounds(visibleItem("channelCancelButton")->parentItem()->parentItem())},
                {"mutationsAtCapture", fixture.posts + fixture.memberPosts},
                {"selectedMembers", QJsonArray{channelPeer}}, {"submittedAtCapture", false}};
            QFile file(base + "-metrics.json"); QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(metrics).toJson(QJsonDocument::Indented));
        }
        click(window, submit);
        QTRY_COMPARE(fixture.posts, 1);
        QVERIFY(!name->isEnabled());
        QVERIFY(!description->isEnabled());
        QVERIFY(!submit->isEnabled());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(fixture.posts, 1);
        QTRY_COMPARE(controller.channelCreationPhase(), QString("ready"));
        QTRY_COMPARE(complete.size(), 1);
        QTRY_VERIFY(!visibleItem("channelNameInput"));
        const auto id = fixture.created.value("uuid").toString();
        QCOMPARE(controller.selectedStream(), id);
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(controller.conversationKind(), QString("stream"));
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.canAttach());
        QTRY_COMPARE(fixture.historyGets - initialHistory, 1);
        QCOMPARE(fixture.canonicalGets, 1);
        QCOMPARE(fixture.topicGets, 1);
        QCOMPARE(fixture.catalogGets - initialCatalogs, 1);
        QCOMPARE(fixture.usersGets, initialUsers);
        QCOMPARE(fixture.memberPosts, scenario == "empty" ? 0 : 1);
        QCOMPARE(fixture.bindingGets, 0);
        QCOMPARE(fixture.streamBody, QJsonObject({{"uuid", id}, {"name", "Native planning"},
            {"description", "Ordinary channel reference"}, {"source_name", "native"},
            {"invite_only", true}, {"announce", true}, {"encryption", false},
            {"current_encryption_key_uuid", QJsonValue::Null}}));
        if (scenario != "empty")
            QCOMPARE(fixture.memberBody, QJsonObject({{"member", QJsonArray{channelPeer}}}));
        QVERIFY(visibleItem("streamTopicPrompt"));
        window->close();
    }

    void globalChannelCreationRecoversTheSameIntent_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"cancel-pending", "uncertain-stream", "early404", "members-failed",
                                    "members-ambiguous-applied", "members-ambiguous-missing", "route", "benign-updates"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void globalChannelCreationRecoversTheSameIntent()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        WindowChannelFixture fixture;
        if (scenario == "uncertain-stream" || scenario == "early404") fixture.postStatus = 503;
        if (scenario == "early404") fixture.canonicalStatus = 404;
        if (scenario == "members-failed") { fixture.memberStatus = 403; fixture.applyMembers = false; }
        if (scenario.startsWith("members-ambiguous")) {
            fixture.memberStatus = 503;
            fixture.applyMembers = scenario == "members-ambiguous-applied";
        }
        if (scenario == "cancel-pending" || scenario == "route") fixture.postDelay = 450;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateChannel() && controller.channelMembersVerified());
        auto *content = window->contentItem();
        openChannelNewChat(window);
        QTRY_VERIFY(visibleItem("channelNameInput"));
        visibleItem("channelNameInput")->setProperty("text", "Same channel intent");
        click(window, visibleItem("channel-member-" + channelPeer));
        if (scenario == "benign-updates") {
            const auto context = controller.channelCreationContextKey();
            const QJsonObject renamed{{"uuid", channelPeer}, {"username", "alice"},
                {"first_name", "<b>Alice</b>"}, {"last_name", "Updated"}, {"email", "alice@example.invalid"},
                {"kind", "user.updated"}};
            QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
                {"uuid", "84000000-0000-4000-8000-000000000001"}, {"payload", renamed}}));
            auto stream = windowChannel(streamId, "Product team");
            stream["unread_count"] = 1;
            stream["kind"] = "stream.updated";
            QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
                {"uuid", "84000000-0000-4000-8000-000000000002"}, {"payload", stream}}));
            QCOMPARE(controller.channelCreationContextKey(), context);
            QVERIFY(visibleItem("channelNameInput"));
            QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString("Same channel intent"));
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList(), QVariantList{channelPeer});
            QVERIFY(visibleItem("channel-member-" + channelPeer)->property("checked").toBool());
            auto *label = visibleItem("channel-name-" + channelPeer);
            QVERIFY(label);
            QCOMPARE(label->property("textFormat").toInt(), 0);
            QCOMPARE(label->property("text").toString(), QString("<b>Alice</b> Updated"));
        }
        click(window, visibleItem("channelCreateButton"));
        QTRY_COMPARE(fixture.posts, 1);
        const auto id = fixture.created.value("uuid").toString();
        if (scenario == "cancel-pending" || scenario == "route") {
            QVERIFY(controller.channelCreationBusy());
            if (scenario == "cancel-pending") click(window, visibleItem("channelCancelButton"));
            else controller.selectStream(streamId);
            QTRY_VERIFY(!visibleItem("channelNameInput"));
            openChannelNewChat(window);
            QTRY_VERIFY(visibleItem("channelNameInput"));
            QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString("Same channel intent"));
            QVERIFY(!visibleItem("channelNameInput")->isEnabled());
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList(), QVariantList{channelPeer});
        }
        QTRY_VERIFY(!controller.channelCreationBusy());
        if (scenario == "members-failed") {
            QCOMPARE(controller.channelCreationPhase(), QString("members-failed"));
            QTRY_VERIFY(visibleItem("channelCreationStatus"));
            auto *retry = visibleItem("channelRetryMembersButton");
            QVERIFY(retry && retry->isEnabled());
            QCOMPARE(retry->property("text").toString(), QString("Retry members"));
            QVERIFY(!visibleItem("channelCreateButton"));
            fixture.memberStatus = 200;
            fixture.applyMembers = true;
            click(window, retry);
        } else if (controller.channelCreationPhase() == "uncertain") {
            QVERIFY(visibleItem("channelCheckButton"));
            QVERIFY(!visibleItem("channelCreateButton"));
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!visibleItem("channelNameInput"));
            openChannelNewChat(window);
            QTRY_VERIFY(visibleItem("channelCheckButton"));
            QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString("Same channel intent"));
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList(), QVariantList{channelPeer});
            const auto postsBefore = fixture.memberPosts;
            click(window, visibleItem("channelCheckButton"));
            QTRY_VERIFY(!controller.channelCreationBusy());
            QCOMPARE(fixture.memberPosts, postsBefore);
            QCOMPARE(fixture.posts, 1);
            if (scenario == "early404") {
                QCOMPARE(controller.channelCreationPhase(), QString("uncertain"));
                fixture.canonicalStatus = 200;
                click(window, visibleItem("channelCheckButton"));
                QTRY_VERIFY(!controller.channelCreationBusy());
                QCOMPARE(fixture.memberPosts, postsBefore);
            }
            if (controller.channelCreationPhase() == "members-required") {
                QTRY_VERIFY(visibleItem("channelCreationStatus"));
                auto *add = visibleItem("channelRetryMembersButton");
                QVERIFY(add && add->isEnabled());
                QCOMPARE(add->property("text").toString(), QString("Add members"));
                fixture.memberStatus = 200;
                fixture.applyMembers = true;
                click(window, add);
            }
        }
        QTRY_COMPARE(controller.channelCreationPhase(), QString("ready"));
        QTRY_COMPARE(complete.size(), 1);
        QTRY_VERIFY(!visibleItem("channelNameInput"));
        QCOMPARE(fixture.posts, 1);
        QCOMPARE(controller.selectedStream(), id);
        QCOMPARE(controller.channelCreationForm().value("attemptId").toString(), id);
        QCOMPARE(fixture.memberBody, QJsonObject({{"member", QJsonArray{channelPeer}}}));
        QCOMPARE(fixture.memberPosts, scenario == "members-failed" || scenario == "members-ambiguous-missing" ? 2 : 1);
        QVERIFY(controller.selectedTopic().isEmpty());
        window->close();
    }

    void globalChannelRealtimeKeepsTheCorrectFormLifetime_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"own-stream", "own-default", "own-bindings", "access-loss",
                                    "stream-deleted", "default-deleted", "newer-default", "logout-pending"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void globalChannelRealtimeKeepsTheCorrectFormLifetime()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        WindowChannelFixture fixture;
        if (scenario == "own-stream") fixture.canonicalDelay = 350;
        if (scenario == "own-default") fixture.topicDelay = 350;
        if (scenario == "own-bindings" || scenario.endsWith("deleted")) fixture.catalogDelay = 350;
        if (scenario == "logout-pending") fixture.postDelay = 450;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QSignalSpy complete(&controller, &NativeController::channelCreated);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateChannel() && controller.channelMembersVerified());
        const auto event = [&](const QString &kind, QJsonObject payload) {
            payload["kind"] = kind;
            return controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
                {"uuid", "85000000-0000-4000-8000-000000000001"}, {"payload", payload}});
        };
        bool boundaryInjected = false;
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (boundaryInjected || controller.channelCreationPhase() != "adding-members") return;
            if (scenario == "access-loss") {
                boundaryInjected = true;
                auto changed = fixture.created;
                changed["role"] = "member";
                QVERIFY(event("stream.updated", changed));
            } else if (scenario == "newer-default") {
                boundaryInjected = true;
                fixture.defaultTopic["name"] = "Newer verified topic";
                QVERIFY(event("topic.updated", fixture.defaultTopic));
            }
        });
        auto *content = window->contentItem();
        openChannelNewChat(window);
        QTRY_VERIFY(visibleItem("channelNameInput"));
        visibleItem("channelNameInput")->setProperty("text", "Realtime ordinary channel");
        click(window, visibleItem("channel-member-" + channelPeer));
        click(window, visibleItem("channelCreateButton"));
        QTRY_COMPARE(fixture.posts, 1);
        const auto originalId = fixture.created.value("uuid").toString();
        if (scenario == "logout-pending") {
            QVERIFY(controller.channelCreationBusy());
            session.logout();
            QTRY_VERIFY(!visibleItem("channelNameInput"));
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList().size(), 0);
            QCOMPARE(engine.rootObjects().first()->property("channelShellContext").toString(), QString{});
            QTest::qWait(500); // The already submitted POST is allowed to settle, without re-entering UI.
            QCOMPARE(complete.size(), 0);
            QVERIFY(controller.channelCreationForm().isEmpty());
            QCOMPARE(fixture.memberPosts, 0);
            QVERIFY(controller.selectedStream().isEmpty());
        } else if (scenario.startsWith("own-")) {
            if (scenario == "own-stream") {
                QTRY_COMPARE(fixture.canonicalGets, 1);
                QVERIFY(event("stream.created", fixture.created));
            } else if (scenario == "own-default") {
                QTRY_COMPARE(fixture.topicGets, 1);
                QVERIFY(event("topic.created", fixture.defaultTopic));
            } else {
                QTRY_COMPARE(fixture.catalogGets, 2);
                QVERIFY(event("stream_bindings.created", {{"uuid", originalId}, {"items", fixture.bindings}}));
            }
            // Matching local creation events are ordinary interleaving, not an ACL change.
            QVERIFY(visibleItem("channelNameInput"));
            QTRY_COMPARE(complete.size(), 1);
            QTRY_VERIFY(!visibleItem("channelNameInput"));
            QCOMPARE(controller.channelCreationPhase(), QString("ready"));
            QCOMPARE(fixture.memberPosts, 1);
            QVERIFY(fixture.canonicalGets <= 2 && fixture.topicGets <= 2 && fixture.catalogGets <= 3);
        } else if (scenario.endsWith("deleted")) {
            QTRY_COMPARE(fixture.catalogGets, 2);
            if (scenario == "stream-deleted") {
                fixture.includeCreated = false;
                QVERIFY(event("stream.deleted", {{"uuid", originalId}}));
            } else {
                fixture.created["default_topic_uuid"] = QJsonValue::Null;
                fixture.includeDefault = false;
                QVERIFY(event("topic.deleted", {{"uuid", channelDefault}, {"stream_uuid", originalId}}));
            }
            QTRY_VERIFY(!controller.channelCreationBusy());
            QTRY_VERIFY(!visibleItem("channelNameInput"));
            QCOMPARE(complete.size(), 0);
            openChannelNewChat(window);
            QTRY_VERIFY(visibleItem("channelNameInput"));
            if (scenario == "stream-deleted") {
                QCOMPARE(controller.channelCreationPhase(), QString("failed"));
                QVERIFY(visibleItem("channelNameInput")->isEnabled());
                QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString{});
                fixture.includeCreated = true;
                fixture.catalogDelay = 0;
                visibleItem("channelNameInput")->setProperty("text", "Explicit new channel");
                click(window, visibleItem("channelCreateButton"));
                QTRY_COMPARE(complete.size(), 1);
                QCOMPARE(fixture.posts, 2);
                QVERIFY(fixture.created.value("uuid").toString() != originalId);
            } else {
                QCOMPARE(visibleItem("channelNameInput")->property("text").toString(), QString("Realtime ordinary channel"));
                QVERIFY(!visibleItem("channelNameInput")->isEnabled());
                QTRY_VERIFY(visibleItem("channelCheckButton"));
                click(window, visibleItem("channelCheckButton"));
                QTRY_VERIFY(!controller.channelCreationBusy());
                QCOMPARE(fixture.posts, 1);
                QCOMPARE(fixture.memberPosts, 1);
                QVERIFY(controller.channelCreationUncertain());
                fixture.created["default_topic_uuid"] = returnedId;
                fixture.defaultTopic["uuid"] = returnedId;
                fixture.includeDefault = true;
                fixture.catalogDelay = 0;
                click(window, visibleItem("channelCheckButton"));
                QTRY_COMPARE(complete.size(), 1);
                QCOMPARE(controller.channelCreationForm().value("defaultTopicId").toString(), returnedId);
            }
        } else if (scenario == "access-loss") {
            QTRY_VERIFY(boundaryInjected);
            QTRY_VERIFY(!controller.channelCreationBusy());
            QTRY_VERIFY(!visibleItem("channelNameInput"));
            QCOMPARE(engine.rootObjects().first()->property("channelSelectedMembers").toList().size(), 0);
            QCOMPARE(complete.size(), 0);
            QCOMPARE(fixture.memberPosts, 0);
            QVERIFY(!controller.canRetryChannelMembers());
        } else {
            QTRY_VERIFY(boundaryInjected);
            QTRY_VERIFY(!controller.channelCreationBusy());
            QCOMPARE(complete.size(), 0);
            QVERIFY(controller.channelCreationUncertain());
            QTRY_VERIFY(visibleItem("channelCheckButton"));
            const auto members = fixture.memberPosts;
            click(window, visibleItem("channelCheckButton"));
            QTRY_VERIFY(!controller.channelCreationBusy());
            QCOMPARE(fixture.posts, 1);
            QCOMPARE(fixture.memberPosts, members);
            if (controller.channelCreationPhase() == "members-required")
                click(window, visibleItem("channelRetryMembersButton"));
            QTRY_COMPARE(complete.size(), 1);
            QCOMPARE(controller.topics().first().toMap().value("name").toString(), QString("Newer verified topic"));
        }
        if (complete.size() == 1) {
            QCOMPARE(controller.selectedStream(), fixture.created.value("uuid").toString());
            QVERIFY(controller.selectedTopic().isEmpty());
            QVERIFY(!visibleItem("channelNameInput"));
        }
        if (scenario != "stream-deleted") QCOMPARE(fixture.posts, 1);
        window->close();
    }

    void globalChannelInputsUseUnicodeCodePoints_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"name-astral", "name-too-long", "description-astral", "description-too-long"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void globalChannelInputsUseUnicodeCodePoints()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        WindowChannelFixture fixture;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateChannel());
        openChannelNewChat(window);
        QTRY_VERIFY(visibleItem("channelNameInput"));
        const bool invalid = scenario.endsWith("too-long");
        const bool description = scenario.startsWith("description");
        const auto text = QString::fromUtf8("😀").repeated((description ? 10000 : 255) + (invalid ? 1 : 0));
        visibleItem("channelNameInput")->setProperty("text", description ? QString("Unicode description") : text);
        if (description) visibleItem("channelDescriptionInput")->setProperty("text", text);
        auto *field = visibleItem(description ? "channelDescriptionInput" : "channelNameInput");
        QCOMPARE(field->property("text").toString().size(), text.size());
        click(window, visibleItem("channelCreateButton"));
        if (invalid) {
            QTRY_VERIFY(!controller.channelCreationError().isEmpty());
            QCOMPARE(fixture.posts, 0);
            QVERIFY(field->isEnabled());
            QCOMPARE(field->property("text").toString().size(), text.size());
            auto *error = visibleItem("channelCreationError");
            QVERIFY(error);
            QCOMPARE(error->property("textFormat").toInt(), 0);
            QCOMPARE(error->property("color").value<QColor>(), QColor("#d92d20"));
        } else {
            QTRY_COMPARE(controller.channelCreationPhase(), QString("ready"));
            QCOMPARE(fixture.posts, 1);
            QCOMPARE(fixture.streamBody.value(description ? "description" : "name").toString(), text);
        }
        window->close();
    }

    void globalChannelFormKeepsNarrowKeyboardFieldsVisible()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        WindowChannelFixture fixture;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        window->resize(700, 600);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateChannel() && controller.channelMembersVerified());
        openChannelNewChat(window);
        QTRY_VERIFY(visibleItem("channelNameInput"));
        auto *dialog = engine.rootObjects().first()->findChild<QObject *>("createChatDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->property("width").toReal(), qreal(700));
        QCOMPARE(dialog->property("height").toReal(), qreal(512));
        auto *body = visibleItem("channelFormBody");
        auto *cancel = visibleItem("channelCancelButton");
        QVERIFY(body && cancel);
        QCOMPARE(body->height(), qreal(375.984375));
        QCOMPARE(body->property("contentHeight").toReal(), qreal(600.53125));
        // Column positioning is polished asynchronously. Observe the actual
        // source-sized narrow footer before testing whether focus can move it.
        QTRY_COMPARE(dialog->property("y").toReal(), qreal(44));
        QTRY_COMPARE(cancel->mapToScene(QPointF(0, 0)).y(), qreal(511));
        const auto footerY = cancel->mapToScene(QPointF(0, 0)).y();
        QQuickItem *search = nullptr;
        for (int steps = 0; steps < 12; ++steps) {
            QTest::keyClick(window, Qt::Key_Tab);
            search = visibleItem("channelMemberSearch");
            if (search && search->hasActiveFocus()) break;
        }
        QVERIFY(search && search->hasActiveFocus());
        QTRY_VERIFY2(body->property("contentY").toReal() > 0,
            "Keyboard focus must reveal source channel fields inside the narrow scroll body");
        QVERIFY(search->mapToScene(QPointF(0, search->height())).y() <= footerY - 12);
        QCOMPARE(cancel->mapToScene(QPointF(0, 0)).y(), footerY);
        QCOMPARE(fixture.posts, 0);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!visibleItem("channelNameInput"));
        window->close();
    }

    void sidebarChannelOffersTopicCreationWithoutChangingSelection()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        MockServer server;
        int creates = 0;
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Product team"}, {"encryption", false}}}};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) {
                if (request.method == "POST") ++creates;
                return Response{QJsonArray{}};
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        auto *content = window->contentItem();
        QTRY_VERIFY(item(content, "stream-" + streamId));
        auto *stream = item(content, "stream-" + streamId);
        QTest::mouseClick(window, Qt::RightButton, {},
            stream->mapToScene(QPointF(20, stream->height() / 2)).toPoint());
        QTRY_VERIFY2(visibleItem("streamNewTopicAction"),
            "The source exposes New topic from the channel context menu");
        QCOMPARE(controller.selectedStream(), QString());
        QCOMPARE(creates, 0);
        QVERIFY(item(content, "createChatButton"));
        QCOMPARE(item(content, "createChatButton")->property("help").toString(), QString("New chat"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!visibleItem("streamNewTopicAction"));
        stream->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(visibleItem("streamNewTopicAction"));
        QCOMPARE(controller.selectedStream(), QString());
        QCOMPARE(creates, 0);
        QTest::keyClick(window, Qt::Key_Escape);
        stream->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Menu);
        QTRY_VERIFY(visibleItem("streamNewTopicAction"));
        QCOMPARE(controller.selectedStream(), QString());
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(visibleItem("topicFormClose"));
        QVERIFY(visibleItem("topicFormClose")->hasActiveFocus());
        QVERIFY(visibleItem("topicFormClose")->property("visualFocus").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        stream->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Menu);
        QTRY_VERIFY(visibleItem("streamNewTopicAction"));
        session.logout();
        QTRY_VERIFY(!visibleItem("streamNewTopicAction"));
        QCOMPARE(engine.rootObjects().first()->property("streamMenuTarget").toString(), QString());
        window->close();
    }

    void sidebarTopicCreationUsesCanonicalHttpAndScopedRecovery_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto &scenario : {"selected", "unselected", "denied", "uncertain", "cancel-pending", "route", "logout", "acl", "acl-change", "astral-128", "too-long"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void sidebarTopicCreationUsesCanonicalHttpAndScopedRecovery()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        preferences.setMode("light");
        MockServer server;
        const QString otherStream = "88888888-8888-4888-8888-888888888888";
        const QString target = scenario == "unselected" ? otherStream : streamId;
        int creates = 0, confirmations = 0, catalogs = 0;
        bool permitted = true, confirmApplied = false;
        QString streamRole = "member";
        QJsonObject created;
        auto topic = [](const QString &id, const QString &stream, const QString &name) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream},
                {"user_uuid", userId}, {"name", name}, {"is_done", false}, {"is_default", false},
                {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", QJsonValue::Null}};
        };
        server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) {
                QJsonArray rows;
                if (permitted) rows.append(QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Product team"}, {"encryption", false}, {"role", streamRole}});
                rows.append(QJsonObject{{"uuid", otherStream}, {"project_id", projectId},
                    {"name", "<b>Other team</b>"}, {"encryption", false}});
                return {rows};
            }
            if (path.endsWith("/users/")) return {QJsonArray{}};
            if (path.endsWith("/stream_topics/") && request.method == "POST") {
                ++creates;
                created = topic(request.body.value("uuid").toString(), request.body.value("stream_uuid").toString(), request.body.value("name").toString());
                return {created, scenario == "denied" ? 403 : scenario == "uncertain" ? 503 : 201, 150};
            }
            if (!created.isEmpty() && path.endsWith("/stream_topics/" + created.value("uuid").toString())) {
                ++confirmations;
                if (scenario == "uncertain" && !confirmApplied) return {QJsonObject{}, 404, 120};
                return {created, 200, 120};
            }
            if (path.endsWith("/stream_topics/")) {
                QJsonArray rows;
                const auto selected = QUrlQuery(request.target).queryItemValue("stream_uuid");
                if (selected == streamId) rows.append(topic(topicId, streamId, "Native desktop"));
                if (!created.isEmpty() && selected == created.value("stream_uuid").toString()) {
                    ++catalogs;
                    rows.append(created);
                }
                return {rows};
            }
            if (path.endsWith("/messages/")) return {QJsonArray{}};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
            if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Window did not release focus before engine teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 2);
        controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1);
        controller.selectTopic(topicId);
        QTRY_VERIFY(controller.canCreateTopicIn(target));
        controller.setDraft("Unrelated existing draft");
        auto *content = window->contentItem();
        openStreamTopicForm(window, target);
        auto *name = visibleItem("topicNameInput");
        auto *submit = visibleItem("topicFormSubmit");
        auto *close = visibleItem("topicFormClose");
        QVERIFY(name && submit && close);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(creates, 0);
        QTRY_VERIFY(close->hasActiveFocus());
        if (scenario == "selected") {
            QCOMPARE(close->property("visualFocus").toBool(), false);
            QCOMPARE(submit->opacity(), qreal(0.5));
            const auto api = window->rendererInterface()->graphicsApi();
            qInfo("Topic dialog scenegraph API=%d; software=%d", int(api), int(QSGRendererInterface::Software));
            auto *surface = visibleItem("topicFormSurface");
            QVERIFY(surface);
            if (api == QSGRendererInterface::Software) {
                QVERIFY(!surface->property("shadowsSupported").toBool());
                QVERIFY(!QQmlProperty::read(surface, "layer.enabled").toBool());
                const auto softwareScreenshot = qEnvironmentVariable("CASSI_NATIVE_TOPIC_SOFTWARE_SCREENSHOT");
                if (!softwareScreenshot.isEmpty()) QVERIFY(window->grabWindow().save(softwareScreenshot));
            }
            if (api != QSGRendererInterface::Software && api != QSGRendererInterface::Null && api != QSGRendererInterface::Unknown) {
                const auto shadowMatchesSource = [&] {
                    const auto pixels = window->grabWindow();
                    if (pixels.isNull()) return false;
                    const qreal scale = window->devicePixelRatio();
                    const auto near = pixels.pixelColor(qRound(720 * scale), qRound(577 * scale));
                    const auto tail = pixels.pixelColor(qRound(720 * scale), qRound(600 * scale));
                    const auto far = pixels.pixelColor(qRound(720 * scale), qRound(640 * scale));
                    // Pinned source shadow-xl darkens this flat chat surface
                    // by 14/255 at +4px, and fades before +67px.
                    return far.red() - near.red() >= 10 && far.red() - near.red() <= 24
                        && far.red() - tail.red() <= 5;
                };
                QTRY_VERIFY2(shadowMatchesSource(), "The source modal casts a visible bounded shadow");
            }
        }
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(name->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(visibleItem("topicFormCancel")->hasActiveFocus());
        auto *dialog = engine.rootObjects().first()->findChild<QObject *>("createTopicDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->property("width").toReal(), qreal(448));
        QCOMPARE(dialog->property("height").toReal(), qreal(246.84375));
        QCOMPARE(name->width(), qreal(398));
        QCOMPARE(name->height(), qreal(40.84375));
        QCOMPARE(name->property("font").value<QFont>().pixelSize(), 16);
        QCOMPARE(visibleItem("topicFormTarget")->property("textFormat").toInt(), 0);
        QCOMPARE(visibleItem("topicFormTitle")->property("textFormat").toInt(), 0);
        QCOMPARE(visibleItem("topicFormTarget")->property("text").toString(), target == otherStream ? QString("<b>Other team</b>") : QString("Product team"));
        QVERIFY(!submit->isEnabled());
        name->setProperty("text", "   ");
        QVERIFY(!submit->isEnabled());
        const QString expected = scenario == "astral-128" ? QString::fromUtf8("😀").repeated(128)
            : scenario == "too-long" ? QString("x").repeated(129) : QString("Design review");
        name->setProperty("text", "  " + expected + "  ");
        QVERIFY(submit->isEnabled());
        name->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Return);
        if (scenario == "too-long") {
            QTRY_VERIFY(!controller.topicCreationError().isEmpty());
            QCOMPARE(creates, 0);
            QVERIFY(visibleItem("topicNameInput"));
            QCOMPARE(name->property("text").toString().trimmed(), expected);
            QTest::keyClick(window, Qt::Key_Escape);
            const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
            return;
        }
        QTRY_COMPARE(creates, 1);
        QCOMPARE(created.value("name").toString(), expected);
        QVERIFY(!name->isEnabled());
        QVERIFY(!submit->isEnabled());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(creates, 1);
        if (scenario == "cancel-pending") {
            click(window, visibleItem("topicFormCancel"));
            QVERIFY(!visibleItem("topicNameInput"));
            QVERIFY(controller.topicCreationBusy());
            openStreamTopicForm(window, target);
            QCOMPARE(visibleItem("topicNameInput")->property("text").toString(), expected);
            QVERIFY(!visibleItem("topicNameInput")->isEnabled());
        } else if (scenario == "route") {
            controller.selectStream(otherStream);
            QTRY_VERIFY(!visibleItem("topicNameInput"));
        } else if (scenario == "logout") {
            session.logout();
            QTRY_VERIFY(!visibleItem("topicNameInput"));
        } else if (scenario == "acl" || scenario == "acl-change") {
            if (scenario == "acl") permitted = false;
            else streamRole = "owner";
            controller.refresh();
            QTRY_VERIFY(!visibleItem("topicNameInput"));
        }
        QTRY_VERIFY(!controller.topicCreationBusy());
        if (scenario == "uncertain") {
            QVERIFY(controller.topicCreationUncertain());
            QCOMPARE(confirmations, 0);
            QCOMPARE(submit->property("text").toString(), QString("Check status"));
            QCOMPARE(visibleItem("topicFormError")->property("textFormat").toInt(), 0);
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!visibleItem("topicNameInput"));
            openStreamTopicForm(window, target);
            QCOMPARE(visibleItem("topicNameInput")->property("text").toString(), expected);
            QVERIFY(!visibleItem("topicNameInput")->isEnabled());
            click(window, visibleItem("topicFormSubmit"));
            QTRY_COMPARE(confirmations, 1);
            QTRY_VERIFY(!controller.topicCreationBusy());
            QVERIFY(controller.topicCreationUncertain());
            QCOMPARE(creates, 1);
            confirmApplied = true;
            click(window, visibleItem("topicFormSubmit"));
            QTRY_COMPARE(confirmations, 2);
            QTRY_COMPARE(controller.selectedTopic(), created.value("uuid").toString());
        } else if (scenario == "denied") {
            QVERIFY(!controller.topicCreationUncertain());
            QVERIFY(visibleItem("topicNameInput"));
            QCOMPARE(name->property("text").toString().trimmed(), expected);
            QVERIFY(name->isEnabled());
            QVERIFY(!visibleItem("topicFormError")->property("text").toString().isEmpty());
            QCOMPARE(confirmations, 0);
        } else if (scenario == "route" || scenario == "logout" || scenario == "acl" || scenario == "acl-change") {
            QTest::qWait(180);
            QVERIFY(!visibleItem("topicNameInput"));
            QVERIFY(controller.selectedTopic() != created.value("uuid").toString());
            QCOMPARE(confirmations, 0);
        } else {
            QTRY_COMPARE(controller.selectedTopic(), created.value("uuid").toString());
        }
        if (scenario != "denied" && scenario != "route" && scenario != "logout" && scenario != "acl" && scenario != "acl-change") {
            QTRY_VERIFY(!visibleItem("topicNameInput"));
            QCOMPARE(controller.selectedStream(), target);
            QCOMPARE(confirmations, scenario == "uncertain" ? 2 : 1);
            QCOMPARE(catalogs, 1);
            controller.selectStream(streamId);
            QTRY_VERIFY(!controller.topics().isEmpty());
            controller.selectTopic(topicId);
            QCOMPARE(controller.draft(), QString("Unrelated existing draft"));
        }
        QCOMPARE(creates, 1);
        const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void oldTopicCompletionCannotCloseTheNextSameTargetForm_data()
    {
        QTest::addColumn<bool>("global"); QTest::newRow("sidebar") << false; QTest::newRow("global") << true;
    }
    void oldTopicCompletionCannotCloseTheNextSameTargetForm()
    {
        QFETCH(bool, global);
        QTemporaryDir directory; NativePreferences preferences(directory.filePath("appearance.ini"));
        MockServer server;
        QMap<QString, QJsonObject> created; int posts = 0;
        server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return {QJsonArray{windowChannel(streamId, "Product team")}};
            if (path.endsWith("/users/") || path.endsWith("/messages/")) return {QJsonArray{}};
            if (path.endsWith("/stream_topics/") && request.method == "POST") {
                ++posts;
                const auto uuid = request.body["uuid"].toString();
                auto row = windowChannelTopic(uuid, streamId, request.body["name"].toString());
                row["is_default"] = false; created.insert(uuid, row);
                return {row, posts == 1 ? 201 : 503};
            }
            for (auto it = created.cbegin(); it != created.cend(); ++it)
                if (path.endsWith("/stream_topics/" + it.key())) return {it.value()};
            if (path.endsWith("/stream_topics/")) {
                QJsonArray rows; for (const auto &row : created) rows.append(row); return {rows};
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; QQuickWindow *window = nullptr;
        bool reentered = false; QString oldUuid, nextUuid;
        TopicCompletionProbe probe;
        connect(&probe, &TopicCompletionProbe::completed, &controller, [&](const QString &, const QString &uuid) {
            if (reentered) return;
            reentered = true; oldUuid = uuid;
            controller.createTopic(streamId, "Same name");
            nextUuid = controller.topicCreationAttemptId();
            if (global) openGlobalTopic(window); else openStreamTopicForm(window, streamId);
        });
        // Install the earlier observer in the same QML notifier chain as Main.
        engine.rootContext()->setContextProperty("nativeController", &controller);
        engine.rootContext()->setContextProperty("nativeTopicProbe", &probe);
        QQmlComponent observer(&engine);
        observer.setData("import QtQuick\nConnections { target: nativeController; function onTopicCreated(stream, uuid) { nativeTopicProbe.completed(stream, uuid); } }", QUrl());
        std::unique_ptr<QObject> observerObject(observer.create());
        QVERIFY2(observerObject, qPrintable(observer.errorString()));
        loadNativeUi(engine, &controller, &preferences);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.canCreateTopicIn(streamId));
        if (global) openGlobalTopic(window); else openStreamTopicForm(window, streamId);
        const auto inputName = global ? "globalTopicNameInput" : "topicNameInput";
        visibleItem(inputName)->setProperty("text", "Same name");
        click(window, visibleItem(global ? "globalTopicCreateButton" : "topicFormSubmit"));
        QTRY_VERIFY(reentered);
        QTRY_COMPARE(posts, 2);
        QTRY_VERIFY(controller.topicCreationUncertain());
        QVERIFY(!nextUuid.isEmpty() && nextUuid != oldUuid);
        QVERIFY2(visibleItem(inputName), "An older same-stream completion must not close the next explicit UUID intent");
        QCOMPARE(window->property(global ? "globalTopicTarget" : "topicFormTarget").toString(), streamId);
        QVERIFY(window->findChild<QObject *>(global ? "createChatDialog" : "createTopicDialog")->property("opened").toBool());
        QCOMPARE(window->property(global ? "globalTopicAttempt" : "topicFormAttempt").toString(), nextUuid);
        QCOMPARE(visibleItem(inputName)->property("text").toString(), QString("Same name"));
        window->close();
    }

    void aggregateStreamUsesSourceGroupsAndTopicChoices_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"en-light", "en-dark", "ru-light", "ru-dark",
                                    "identity-day", "visible-reads"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void aggregateStreamUsesSourceGroupsAndTopicChoices()
    {
        QFETCH(QString, scenario);
        const bool readScenario = scenario == "visible-reads";
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setMode(scenario.endsWith("dark") ? "dark" : "light");
        preferences.setLocale(scenario.startsWith("ru") ? "ru" : "en");
        const QString peerId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QString otherPeerId = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        const QString closedTopicId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        const QString unknownTopicId = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        const auto idFor = [](int number) {
            return QString("81000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'));
        };
        QJsonArray history;
        const QStringList contents{
            "General discussion stays in this channel view.",
            "A second message from the same author and topic.",
            "Planning now starts a new topic run.",
            "Own messages carry the topic label once per group.",
            "A resolved topic remains visible in the aggregate.",
            "Returning to Planning adds another divider.",
            "A message with missing topic metadata has no invented label."};
        const QStringList rowTopics{topicId, topicId, secondTopicId, secondTopicId,
                                    closedTopicId, secondTopicId, unknownTopicId};
        const int size = readScenario ? 400 : scenario == "identity-day" ? 10 : 7;
        for (int number = 1; number <= size; ++number) {
            const auto topic = readScenario ? (number % 2 ? topicId : secondTopicId)
                : number <= 7 ? rowTopics[number - 1] : unknownTopicId;
            auto row = message(readScenario
                ? QString("Visible topic fixture %1\nLine two\nLine three").arg(number)
                : number <= 7 ? contents[number - 1] : QString("Identity/day boundary %1").arg(number),
                idFor(number), topic);
            const bool own = !readScenario && (number == 4 || number == 5);
            row["author_uuid"] = own ? userId : number >= 8 ? otherPeerId : peerId;
            row["is_own"] = own;
            row["read"] = !readScenario;
            row["created_at"] = QDateTime::fromString("2026-01-02T10:00:00.000Z", Qt::ISODateWithMs)
                .addSecs((number - 1) * 60 + (number >= 9 ? 86400 : 0)).toString(Qt::ISODateWithMs);
            history.append(row);
        }
        const auto unread = [&](const QString &topic = {}) {
            int count = 0;
            for (const auto &value : history) {
                const auto row = value.toObject();
                if ((topic.isEmpty() || row["topic_uuid"] == topic) && !row["read"].toBool()) ++count;
            }
            return count;
        };
        const auto topic = [&](const QString &id, const QString &name) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", scenario == "identity-day" && id == secondTopicId ? "<b>Planning</b>" : name},
                {"is_done", id == closedTopicId}, {"is_default", id == topicId},
                {"unread_count", readScenario ? unread(id) : id == closedTopicId ? 3 : 0},
                {"updated_at", "2026-01-02T10:00:00.000Z"}};
        };
        QStringList readAnchors;
        int aggregateGets = 0;
        MockServer server;
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/") || path.endsWith("/streams/" + streamId)) {
                const QJsonObject stream{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Product team"}, {"description", "Native preview"}, {"encryption", false},
                    {"unread_count", readScenario ? unread() : 3},
                    {"updated_at", "2026-01-02T10:00:00.000Z"}};
                return path.endsWith('/') ? Response{QJsonArray{stream}} : Response{stream};
            }
            if (path.endsWith("/users/"))
                return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", "Cassi"}},
                    QJsonObject{{"uuid", peerId}, {"username", "Alice"}},
                    QJsonObject{{"uuid", otherPeerId}, {"username", "Alice"}}}};
            if (path.endsWith("/stream_topics/"))
                return Response{QJsonArray{topic(topicId, "General Topic"),
                    topic(secondTopicId, "Planning"), topic(closedTopicId, "Releases")}};
            for (const auto &id : {topicId, secondTopicId, closedTopicId})
                if (path.endsWith("/stream_topics/" + id))
                    return Response{topic(id, id == topicId ? "General Topic"
                        : id == secondTopicId ? "Planning" : "Releases")};
            if (path.endsWith("/messages/") && request.method == "GET") {
                const QUrlQuery query(request.target);
                if (!query.hasQueryItem("topic_uuid")) ++aggregateGets;
                const auto marker = query.queryItemValue("page_marker");
                QJsonArray page;
                bool more = false;
                for (auto it = history.constEnd(); it != history.constBegin();) {
                    const auto row = (--it)->toObject();
                    if (!marker.isEmpty() && row["uuid"].toString() >= marker) continue;
                    if (!query.hasQueryItem("topic_uuid") || row["topic_uuid"] == query.queryItemValue("topic_uuid")) {
                        if (readScenario && page.size() == 50) { more = true; break; }
                        page.append(row);
                    }
                }
                Response response{page};
                if (more) response.headers.insert("X-Pagination-Marker", page.last().toObject()["uuid"].toString().toUtf8());
                return response;
            }
            if (path.endsWith("/actions/read_up_to/invoke")) {
                const auto id = path.section('/', -4, -4);
                int boundary = -1;
                for (int i = 0; i < history.size(); ++i)
                    if (history[i].toObject()["uuid"] == id) boundary = i;
                if (boundary < 0) return Response{QJsonObject{}, 404};
                readAnchors.append(id);
                const auto actualTopic = history[boundary].toObject().value("topic_uuid").toString();
                for (int i = 0; i <= boundary; ++i) {
                    auto row = history[i].toObject();
                    if (row["topic_uuid"] == actualTopic) {
                        row["read"] = true;
                        history[i] = row;
                    }
                }
                return Response{history[boundary]};
            }
            auto response = sessionResponse(request);
            if (path.endsWith("/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", QJsonObject{
                    {"uuid", userId}, {"username", "cassi"}, {"first_name", "Cassandra"},
                    {"last_name", "Volkova"}, {"email", "cassi@example.invalid"}}}};
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.setRealtimeMode(true);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QQuickWindow other;
        if (readScenario) {
            other.setTitle("Synthetic aggregate focus target");
            other.resize(300, 200);
            other.show();
            other.requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(&other));
        }
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        controller.reloadSnapshot();
        QTRY_COMPARE(controller.streams().size(), 1);
        if (readScenario) controller.selectStream(streamId);
        else click(window, item(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 3);
        // This was the first actual UI RED: stream selection had no source
        // topic-choice footer and retained the disabled composer frame instead.
        QTRY_VERIFY(visibleItem("streamTopicPrompt"));
        QTRY_COMPARE(controller.messages().size(), readScenario ? 50 : size);
        QCOMPARE(controller.property("conversationKind").toString(), QString("stream"));
        QVERIFY(controller.property("hasConversation").toBool());
        QVERIFY(aggregateGets > 0);
        QVERIFY(!visibleItem("messageComposerShell"));
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.canAttach());
        auto *list = item(window->contentItem(), "messageList");
        QVERIFY(list);
        QTRY_VERIFY(!list->property("positioning").toBool());
        if (readScenario) {
            for (int loaded = 50; loaded < size; loaded += 50) {
                QVERIFY(controller.canLoadOlder());
                controller.loadOlder();
                QTRY_COMPARE(controller.messages().size(), loaded + 50);
            }
            QTRY_VERIFY(!list->property("positioning").toBool());
            QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
            QTest::qWait(400);
            QVERIFY(readAnchors.isEmpty());
            const auto instantiated = itemCountWithPrefix(window->contentItem(), "message-avatar-")
                - itemCountWithPrefix(window->contentItem(), "message-avatar-initial-");
            QVERIFY2(instantiated < 64, "A 400-row loaded window must retain a bounded virtual delegate set");
            QMap<QString, QString> expected;
            for (const auto &value : controller.messages()) {
                const auto row = value.toMap();
                auto *bubble = item(window->contentItem(), "message-" + row["id"].toString());
                if (!bubble || !bubble->isVisible()) continue;
                const auto point = bubble->mapToItem(list, QPointF());
                if (point.y() < list->height() && point.y() + bubble->height() > 0)
                    expected[row["topicId"].toString()] = row["id"].toString();
            }
            QCOMPARE(expected.size(), 2);
            qInfo("Aggregate viewport: loaded=%d, instantiated=%d, visible topics=%d",
                static_cast<int>(controller.messages().size()), instantiated, static_cast<int>(expected.size()));
            other.hide();
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
            QTRY_COMPARE_WITH_TIMEOUT(readAnchors.size(), 2, 1500);
            QSet<QString> actual(readAnchors.begin(), readAnchors.end());
            const auto values = expected.values();
            QCOMPARE(actual, QSet<QString>(values.begin(), values.end()));
            QVERIFY(!controller.messages().last().toMap()["read"].toBool());
            QTest::qWait(400);
            QCOMPARE(readAnchors.size(), 2);
            other.show();
            other.requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(&other));
            QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
            window->showMinimized();
            QTest::qWait(400);
            QCOMPARE(readAnchors.size(), 2);
            other.close();
            window->close();
            return;
        }
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        QTRY_VERIFY(item(window->contentItem(), "message-topic-" + idFor(1)));
        QCOMPARE(item(window->contentItem(), "message-topic-" + idFor(1))->property("text").toString(), QString("#General Topic"));
        auto *topicText = item(window->contentItem(), "message-topic-" + idFor(1))->property("contentItem").value<QQuickItem *>();
        QVERIFY(topicText);
        qInfo("Topic label width %.3f implicit %.3f button %.3f", topicText->width(), topicText->implicitWidth(), topicText->parentItem()->width());
        QVERIFY2(!topicText->property("truncated").toBool(), "A short source topic label must fit its bubble header");
        auto *compactPeer = item(window->contentItem(), "message-" + idFor(1));
        QVERIFY(compactPeer);
        QCOMPARE(compactPeer->property("topLeftRadius").toReal(), 10);
        QCOMPARE(compactPeer->property("bottomLeftRadius").toReal(), 10);
        QCOMPARE(compactPeer->property("topRightRadius").toReal(), 18);
        QVERIFY(!visibleItem("message-topic-" + idFor(2)));
        QVERIFY(!visibleItem("message-author-" + idFor(2)));
        QVERIFY(!visibleItem("message-avatar-" + idFor(1)));
        QVERIFY(visibleItem("message-avatar-" + idFor(2)));
        QVERIFY(!visibleItem("message-divider-" + idFor(1)));
        for (int number : {3, 5, 6, 7}) QVERIFY(visibleItem("message-divider-" + idFor(number)));
        QVERIFY(!visibleItem("message-divider-" + idFor(4)));
        QVERIFY(!visibleItem("message-topic-" + idFor(7)));
        QCOMPARE(item(window->contentItem(), "message-divider-" + idFor(3))->height(), 24);
        QCOMPARE(item(window->contentItem(), "message-divider-" + idFor(7))->height(), 9);
        QCOMPARE(visibleItem("streamTopicPrompt")->height(), 57);
        auto *shortcut = item(window->contentItem(), "topicShortcut-" + closedTopicId);
        QVERIFY(shortcut && shortcut->isEnabled());
        QCOMPARE(shortcut->height(), 36);
        QCOMPARE(item(window->contentItem(), "topicShortcutText-" + closedTopicId)->property("font").value<QFont>().strikeOut(), true);
        auto *planningShortcut = item(window->contentItem(), "topicShortcut-" + secondTopicId);
        auto *planningText = item(window->contentItem(), "topicShortcutText-" + secondTopicId);
        QVERIFY(planningShortcut && planningText);
        qInfo("Shortcut label width %.3f implicit %.3f button %.3f", planningText->width(), planningText->implicitWidth(), planningShortcut->width());
        QVERIFY2(!planningText->property("truncated").toBool(), "A short source topic shortcut must not be elided");
        if (scenario == "identity-day") {
            auto *plainTopic = item(window->contentItem(), "message-topic-" + idFor(3));
            QVERIFY(plainTopic);
            QCOMPARE(plainTopic->property("text").toString(), QString("#<b>Planning</b>"));
            auto *plainText = plainTopic->property("contentItem").value<QQuickItem *>();
            QVERIFY(plainText);
            QCOMPARE(plainText->property("textFormat").toInt(), 0);
            QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
            QTRY_VERIFY(visibleItem("message-author-" + idFor(8)));
            QVERIFY(visibleItem("message-author-" + idFor(9)));
            QVERIFY(!visibleItem("message-author-" + idFor(10)));
            QVERIFY(!visibleItem("message-divider-" + idFor(8)));
            QVERIFY(!visibleItem("message-divider-" + idFor(9)));
        } else {
            // Source full-frame group positions relative to the same chat
            // viewport origin, not a wrapper's unrelated desktop coordinate.
            const QList<qreal> sourceY{58, 120, 198, 264, 362, 460, 543};
            QJsonArray metrics;
            for (int number = 1; number <= 7; ++number) {
                auto *bubble = item(window->contentItem(), "message-" + idFor(number));
                QVERIFY(bubble);
                const auto point = bubble->mapToItem(list, QPointF());
                QVERIFY2(qAbs(point.y() - sourceY[number - 1]) <= 1,
                    qPrintable(QString("Source aggregate bubble %1 y=%2, expected=%3")
                        .arg(number).arg(point.y()).arg(sourceY[number - 1])));
                QVERIFY(qAbs(bubble->height() - (number == 2 ? 38 : 58)) <= 1);
                metrics.append(QJsonObject{{"id", idFor(number)}, {"x", point.x()}, {"y", point.y()},
                    {"width", bubble->width()}, {"height", bubble->height()}});
            }
            const auto captureDirectory = qEnvironmentVariable("CASSI_NATIVE_TEST_AGGREGATE_CAPTURE_DIR");
            if (!captureDirectory.isEmpty()) {
                QTest::mouseMove(window, QPoint(1100, 800));
                QTest::qWait(150);
                QVERIFY(window->grabWindow().save(QDir(captureDirectory).filePath("native-aggregate-v15-" + scenario + ".png")));
                QFile output(QDir(captureDirectory).filePath("native-aggregate-v15-" + scenario + "-metrics.json"));
                QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
                output.write(QJsonDocument(QJsonObject{{"synthetic", true}, {"httpVerified", true},
                    {"dateFixtureSeam", false}, {"constructor", "QApplication"},
                    {"viewport", QJsonArray{window->width(), window->height()}},
                    {"devicePixelRatio", window->devicePixelRatio()},
                    {"sceneGraphApi", static_cast<int>(window->rendererInterface()->graphicsApi())},
                    {"fontFamily", QFontInfo(QGuiApplication::font()).family()}, {"bubbles", metrics}}).toJson());
            }
            if (scenario == "en-light") {
                auto *ownBubble = item(window->contentItem(), "message-" + idFor(4));
                QVERIFY(ownBubble);
                QTest::mouseClick(window, Qt::RightButton, {}, ownBubble->mapToScene(QPointF(20, 20)).toPoint());
                QTRY_VERIFY(visibleItem("copyMessageAction"));
                QVERIFY(!visibleItem("replyMessageAction"));
                QVERIFY(visibleItem("editMessageAction"));
                QTest::keyClick(window, Qt::Key_Escape);
            }
            auto *inlineLink = item(window->contentItem(), "message-topic-" + idFor(3));
            if (scenario == "en-dark") {
                inlineLink->forceActiveFocus(Qt::TabFocusReason);
                QTRY_VERIFY(inlineLink->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Space);
            } else click(window, inlineLink);
            QTRY_COMPARE(controller.selectedTopic(), secondTopicId);
            QTRY_VERIFY(visibleItem("messageComposerShell"));
            controller.setDraft("A topic draft survives the aggregate route");
            click(window, item(window->contentItem(), "stream-" + streamId));
            QTRY_VERIFY(visibleItem("streamTopicPrompt"));
            QVERIFY(!visibleItem("messageComposerShell"));
            QTRY_VERIFY(visibleItem("topicShortcut-" + secondTopicId));
            click(window, item(window->contentItem(), "topicShortcut-" + secondTopicId));
            QTRY_COMPARE(controller.selectedTopic(), secondTopicId);
            QCOMPARE(controller.draft(), QString("A topic draft survives the aggregate route"));
            click(window, item(window->contentItem(), "stream-" + streamId));
            QTRY_VERIFY(visibleItem("topicShortcut-" + closedTopicId));
            if (scenario == "ru-dark") {
                window->resize(700, 600);
                auto *rail = item(window->contentItem(), "streamTopicRail");
                QVERIFY(rail);
                QTRY_VERIFY(rail->property("contentWidth").toReal() > rail->width());
                const auto oldX = rail->property("contentX").toReal();
                const auto center = rail->mapToScene(QPointF(rail->width() / 2, rail->height() / 2));
                QTest::wheelEvent(window, center, QPoint(0, -120));
                QTRY_VERIFY(rail->property("contentX").toReal() > oldX);
                rail->setProperty("contentX", 0);
                QTest::wheelEvent(window, center, QPoint(-120, 0));
                QTRY_VERIFY_WITH_TIMEOUT(rail->property("contentX").toReal() > 0, 1000);
                auto *closedShortcut = item(window->contentItem(), "topicShortcut-" + closedTopicId);
                closedShortcut->forceActiveFocus(Qt::TabFocusReason);
                QTRY_VERIFY(closedShortcut->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Space);
            } else click(window, item(window->contentItem(), "topicShortcut-" + closedTopicId));
            QTRY_COMPARE(controller.selectedTopic(), closedTopicId);
            QTRY_VERIFY(controller.topicClosed());
            QVERIFY(!visibleItem("messageComposerShell"));
            QVERIFY(visibleItem("reopenTopicButton"));
        }
        window->close();
    }

    void realQuickWindowSignsInNavigatesAndSends()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        preferences.setMode("light");
        MockServer server;
        QJsonObject saved;
        const QString peerId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        QString directoryName = "Cassi";
        QJsonObject verifiedProfile{{"uuid", userId}, {"username", "cassi"},
            {"first_name", "Cassandra"}, {"last_name", "Volkova"}, {"email", "cassi@example.invalid"}};
        server.handler = [&saved, &verifiedProfile, &peerId, &directoryName](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Product team"}, {"description", "Native preview"}, {"encryption", false}, {"unread_count", 2}}}};
            if (path.endsWith("/users/"))
                return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", directoryName}},
                    QJsonObject{{"uuid", peerId}, {"username", "Cassi"}}}};
            if (path.endsWith("/stream_topics/"))
                return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Native desktop"}, {"unread_count", 2}}}};
            if (path.endsWith("/messages/") && request.method == "GET") {
                auto peer = message("**The native interface is taking shape.**\nLet’s compare _formatting_ and interaction states.");
                peer.insert("author_uuid", peerId);
                peer.insert("is_own", false);
                auto own = message("The layout now uses the same `palette`, spacing and source icons.", returnedId);
                return Response{QJsonArray{own, peer}};
            }
            if (path.endsWith("/messages/") && request.method == "POST") {
                saved = message(request.body.value("payload").toObject().value("content").toString(),
                                "99999999-9999-4999-8999-999999999999");
                return Response{saved};
            }
            if (path.endsWith("/messages/99999999-9999-4999-8999-999999999999"))
                return Response{saved};
            auto response = sessionResponse(request);
            if (path.endsWith("/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", verifiedProfile}};
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *content = window->contentItem();
        auto *serverInput = item(content, "serverInput");
        QVERIFY(serverInput);
        serverInput->setProperty("text", server.origin().toString());
        item(content, "userInput")->setProperty("text", "synthetic-user");
        item(content, "passwordInput")->setProperty("text", "synthetic-password");
        click(window, item(content, "signInButton"));
        QTRY_COMPARE(controller.projects().size(), 1);
        QTRY_VERIFY(item(content, "project-" + projectId));
        click(window, item(content, "project-" + projectId));
        QTRY_COMPARE(controller.streams().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(item(content, "profileDisplayName"), 1000);
        QCOMPARE(item(content, "profileDisplayName")->property("text").toString(), QString("Cassi"));
        QCOMPARE(item(content, "profileEmail")->property("text").toString(), QString("cassi@example.invalid"));
        QCOMPARE(item(content, "profileInitial")->property("text").toString(), QString("C"));
        QCOMPARE(item(content, "profileAvatar")->width(), 36);
        QCOMPARE(item(content, "profileDisplayName")->property("textFormat").toInt(), 0);
        QCOMPARE(item(content, "profileEmail")->property("textFormat").toInt(), 0);
        QTRY_COMPARE(item(content, "organizationLogo")->property("status").toInt(), 1);
        QVERIFY(!item(content, "messageComposer")->isEnabled());
        QTRY_VERIFY(item(content, "stream-" + streamId));
        click(window, item(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        QTRY_VERIFY(item(content, "topic-" + topicId));
        click(window, item(content, "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 2);
        QTRY_VERIFY(item(content, "message-content-" + messageId));
        const auto peerHtml = item(content, "message-content-" + messageId)->property("text").toString();
        QVERIFY(peerHtml.contains(QRegularExpression("font-weight:\\s*700")));
        QVERIFY(peerHtml.contains(QRegularExpression("font-style:\\s*italic")));
        QTRY_VERIFY(item(content, "message-content-" + returnedId));
        const auto ownHtml = item(content, "message-content-" + returnedId)->property("text").toString();
        QVERIFY(ownHtml.contains("monospace"));
        auto *composer = item(content, "messageComposer");
        QVERIFY(composer);
        composer->setProperty("text", "A native message verified through HTTP read-back.");
        QTRY_VERIFY(controller.canSend());
        click(window, item(content, "sendButton"));
        QTRY_VERIFY(controller.draft().isEmpty());
        QTRY_COMPARE(controller.messages().size(), 3);
        QCOMPARE(controller.messages().last().toMap().value("id").toString(),
                 "99999999-9999-4999-8999-999999999999");
        // Measured from the actual source components at the same 1440x900,
        // DPI 1, synthetic content and resolved Ubuntu Sans. Compare bubble
        // dimensions, never the unrelated scene-wrapper origin.
        QTRY_VERIFY(item(content, "message-99999999-9999-4999-8999-999999999999"));
        auto *peerBubble = item(content, "message-" + messageId);
        auto *codeBubble = item(content, "message-" + returnedId);
        auto *plainBubble = item(content, "message-99999999-9999-4999-8999-999999999999");
        QVERIFY(peerBubble && codeBubble && plainBubble);
        QTRY_VERIFY(plainBubble->height() >= 30);
        qInfo("Source fixture measured native bubbles: peer %.3fx%.3f, code %.3fx%.3f, plain %.3fx%.3f", peerBubble->width(), peerBubble->height(), codeBubble->width(), codeBubble->height(), plainBubble->width(), plainBubble->height());
        QTRY_VERIFY2(codeBubble->height() <= 41, "Source inline-code message stays on one line with inline time");
        QVERIFY(qAbs(codeBubble->width() - 479.078125) <= 3);
        QVERIFY(qAbs(plainBubble->width() - 382.703125) <= 3);
        QVERIFY(qAbs(peerBubble->width() - 364.28125) <= 3);
        QVERIFY(qAbs(peerBubble->height() - 78) <= 2);
        auto *time = item(content, "message-time-" + returnedId);
        QVERIFY(time);
        QCOMPARE(time->property("font").value<QFont>().pixelSize(), 12);
        QVERIFY(item(content, "message-content-" + returnedId)->property("contentHeight").toReal() <= 23);
        const bool dateFixtureSeam = qEnvironmentVariableIsSet("CASSI_NATIVE_TEST_DATE_FIXTURE");
        if (dateFixtureSeam) {
            // Explicit visual-reference seam for the frozen pre-createdAt core;
            // all rows retain their verified HTTP UUID/author/content/ownership.
            auto rows = controller.messages();
            for (auto &value : rows) {
                auto entry = value.toMap();
                entry.insert("createdAt", "2026-01-02T10:00:00.000Z");
                value = entry;
            }
            auto *list = item(content, "messageList");
            QVERIFY(QMetaObject::invokeMethod(list, "replaceMessages", Q_ARG(QVariant, QVariant(rows)), Q_ARG(QVariant, QVariant(false))));
            QTRY_VERIFY(!list->property("positioning").toBool());
            QTRY_VERIFY(item(content, "messageDay-2026-01-02"));
        }
        const auto metricsPath = qEnvironmentVariable("CASSI_NATIVE_TEST_METRICS");
        if (!metricsPath.isEmpty()) {
            QJsonArray rows;
            for (const auto &value : controller.messages()) {
                const auto entry = value.toMap();
                const auto id = entry.value("id").toString();
                auto *bubble = item(content, "message-" + id);
                auto *text = item(content, "message-content-" + id);
                QVERIFY(bubble && text);
                const auto point = bubble->mapToScene(QPointF());
                const auto font = text->property("font").value<QFont>();
                rows.append(QJsonObject{{"id", id}, {"own", entry.value("own").toBool()},
                    {"x", point.x()}, {"y", point.y()}, {"width", bubble->width()}, {"height", bubble->height()},
                    {"font", QFontInfo(font).family()}, {"fontPixelSize", font.pixelSize()}});
            }
            QFile metrics(metricsPath);
            QVERIFY(metrics.open(QIODevice::WriteOnly));
            const auto data = QJsonDocument(QJsonObject{{"synthetic", true}, {"dateFixtureSeam", dateFixtureSeam}, {"width", window->width()},
                {"height", window->height()}, {"scale", window->devicePixelRatio()}, {"rows", rows}}).toJson();
            QCOMPARE(metrics.write(data), data.size());
        }
        const auto screenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::mouseMove(window, QPoint(1100, 500));
            QTest::qWait(150);
            QVERIFY(window->grabWindow().save(screenshot));
        }
        const auto darkScreenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_DARK_SCREENSHOT");
        if (!darkScreenshot.isEmpty()) {
            const auto lightColor = window->color();
            click(window, item(content, "accountMenuButton"));
            QTRY_VERIFY(item(content, "appearanceToggle"));
            const QPointer<QQuickItem> appearance = item(content, "appearanceToggle");
            QTRY_VERIFY(appearance->isVisible());
            click(window, appearance);
            QTRY_VERIFY(window->color() != lightColor);
            QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(1100, 500));
            QTRY_VERIFY(!appearance || !appearance->isVisible());
            QTest::mouseMove(window, QPoint(1100, 500));
            QVERIFY(window->grabWindow().save(darkScreenshot));
        }
        const auto reactionScreenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_REACTION_SCREENSHOT");
        const auto reactionDarkScreenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_REACTION_DARK_SCREENSHOT");
        const auto menuScreenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_MENU_SCREENSHOT");
        const auto menuDarkScreenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_MENU_DARK_SCREENSHOT");
        if (!reactionScreenshot.isEmpty() || !reactionDarkScreenshot.isEmpty() || !menuScreenshot.isEmpty() || !menuDarkScreenshot.isEmpty()) {
            preferences.setMode("light");
            auto *bubble = item(content, "message-" + messageId);
            QVERIFY(bubble);
            QTest::mouseMove(window, bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
            QTRY_VERIFY(item(content, "add-reaction-" + messageId)->isVisible());
            if (!menuScreenshot.isEmpty() || !menuDarkScreenshot.isEmpty()) {
                QTest::mouseClick(window, Qt::RightButton, {}, bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
                QTRY_VERIFY(visibleItem("copyMessageAction"));
                QTest::mouseMove(window, QPoint(2, 2));
                if (!menuScreenshot.isEmpty()) QVERIFY(window->grabWindow().save(menuScreenshot));
                if (!menuDarkScreenshot.isEmpty()) {
                    const auto lightColor = window->color();
                    preferences.setMode("dark");
                    QTRY_VERIFY(window->color() != lightColor);
                    QVERIFY(window->grabWindow().save(menuDarkScreenshot));
                    preferences.setMode("light");
                    QTRY_COMPARE(window->color(), lightColor);
                }
                QTest::keyClick(window, Qt::Key_Escape);
                QTRY_VERIFY(!visibleItem("copyMessageAction"));
            }
            openFullReactionPicker(window, bubble);
            auto *grid = item(content, "reactionGrid");
            QTRY_VERIFY(grid && grid->isVisible());
            QTRY_COMPARE(grid->property("emojiCount").toInt(), 1895);
            auto *picker = engine.rootObjects().first()->findChild<QObject *>("reactionPicker");
            QVERIFY(picker);
            QCOMPARE(picker->property("width").toReal(), 320);
            QCOMPARE(picker->property("height").toReal(), 360);
            QTest::mouseMove(window, QPoint(2, 2));
            QTest::qWait(150);
            if (!reactionScreenshot.isEmpty()) QVERIFY(window->grabWindow().save(reactionScreenshot));
            if (!reactionDarkScreenshot.isEmpty()) {
                const auto lightColor = window->color();
                preferences.setMode("dark");
                QTRY_VERIFY(window->color() != lightColor);
                QTest::qWait(150);
                QVERIFY(window->grabWindow().save(reactionDarkScreenshot));
            }
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!grid->isVisible());
        }
        const auto topicScreenshotPrefix = qEnvironmentVariable("CASSI_NATIVE_TOPIC_SCREENSHOT_PREFIX");
        if (!topicScreenshotPrefix.isEmpty()) {
            QJsonArray scenes;
            for (const auto &locale : {QString("en"), QString("ru")}) {
                preferences.setLocale(locale);
                for (const auto &mode : {QString("light"), QString("dark")}) {
                    preferences.setMode(mode);
                    openStreamTopicForm(window, streamId);
                    auto *field = visibleItem("topicNameInput");
                    auto *dialog = engine.rootObjects().first()->findChild<QObject *>("createTopicDialog");
                    QVERIFY(field && dialog);
                    QTRY_VERIFY(visibleItem("topicFormClose")->hasActiveFocus());
                    QCOMPARE(dialog->property("width").toReal(), qreal(448));
                    QCOMPARE(dialog->property("height").toReal(), qreal(246.84375));
                    QCOMPARE(field->width(), qreal(398));
                    QCOMPARE(field->height(), qreal(40.84375));
                    const auto point = field->mapToScene(QPointF());
                    QVERIFY(qAbs(point.x() - 521) <= 1);
                    QVERIFY(qAbs(point.y() - 455.578125) <= 1);
                    const auto prefix = topicScreenshotPrefix + "-" + locale + "-" + mode;
                    QTest::mouseMove(window, QPoint(1100, 500));
                    QTest::qWait(150);
                    QVERIFY(window->grabWindow().save(prefix + "-blank.png"));
                    field->setProperty("text", "Design review");
                    QTRY_VERIFY(visibleItem("topicFormSubmit")->isEnabled());
                    QVERIFY(window->grabWindow().save(prefix + "-named.png"));
                    scenes.append(QJsonObject{{"locale", locale}, {"mode", mode},
                        {"dialogWidth", dialog->property("width").toReal()}, {"dialogHeight", dialog->property("height").toReal()},
                        {"inputX", point.x()}, {"inputY", point.y()}, {"inputWidth", field->width()},
                        {"inputHeight", field->height()}, {"inputFont", QFontInfo(field->property("font").value<QFont>()).family()},
                        {"inputPixelSize", field->property("font").value<QFont>().pixelSize()}});
                    QTest::keyClick(window, Qt::Key_Escape);
                    QTRY_VERIFY(!visibleItem("topicNameInput"));
                }
            }
            QFile metrics(topicScreenshotPrefix + "-metrics.json");
            QVERIFY(metrics.open(QIODevice::WriteOnly));
            const auto data = QJsonDocument(QJsonObject{{"synthetic", true}, {"productionServiceAcceptance", false},
                {"dateFixtureSeam", dateFixtureSeam}, {"width", window->width()}, {"height", window->height()},
                {"scale", window->devicePixelRatio()}, {"scenes", scenes}}).toJson();
            QCOMPARE(metrics.write(data), data.size());
            preferences.setLocale("en");
            preferences.setMode("light");
        }
        verifiedProfile["first_name"] = "<b>Public name</b> " + QString(100, 'W');
        session.requestRefresh();
        QTRY_VERIFY(controller.account().value("displayName").toString().startsWith("<b>Public name</b>"));
        QTRY_COMPARE(item(content, "profileDisplayName")->property("text").toString(), QString("Cassi"));
        directoryName = "<b>Directory name</b> " + QString(100, 'W');
        controller.refresh();
        QTRY_COMPARE(controller.viewerDisplayName(), directoryName);
        QTRY_COMPARE(item(content, "profileDisplayName")->property("text").toString(), directoryName);
        QCOMPARE(item(content, "profileDisplayName")->property("textFormat").toInt(), 0);
        QTRY_VERIFY(item(content, "profileDisplayName")->property("truncated").toBool());
        controller.logout();
        QVERIFY(controller.account().isEmpty());
        QVERIFY(controller.messages().isEmpty());
        window->close();
    }

    void profileFocusRingFollowsKeyboardRatherThanMouseFocus()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        server.handler = [](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/") || path.endsWith("/users/")) return Response{QJsonArray{}};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
            if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Window did not release focus before engine teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(controller.authenticated());
        auto *profile = item(window->contentItem(), "accountMenuButton");
        QVERIFY(profile);
        auto *surface = profile->property("background").value<QQuickItem *>();
        QVERIFY(surface);
        click(window, profile);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(profile->hasActiveFocus());
        QVERIFY(!profile->property("visualFocus").toBool());
        QCOMPARE(QQmlProperty::read(surface, "border.width").toReal(), 0);

        auto *previous = profile->nextItemInFocusChain(false);
        QVERIFY(previous && previous != profile);
        previous->forceActiveFocus(Qt::MouseFocusReason);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(profile->hasActiveFocus());
        QVERIFY(profile->property("visualFocus").toBool());
        QCOMPARE(QQmlProperty::read(surface, "border.width").toReal(), 1);
        QTest::keyClick(window, Qt::Key_Space);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(profile->hasActiveFocus());
        QCOMPARE(QQmlProperty::read(surface, "border.width").toReal(), 1);
        click(window, item(window->contentItem(), "sidebarSearch"));
        QTRY_VERIFY(!profile->hasActiveFocus());
        click(window, profile);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!profile->property("visualFocus").toBool());
        QCOMPARE(QQmlProperty::read(surface, "border.width").toReal(), 0);
        const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void visibleReactionsUseVerifiedOwnershipAndScopedVirtualPicker_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"add-remove", "uncertain-check", "provider-remove", "encrypted-metadata", "picker-fences", "picker-categories-skins", "quick-capsule", "quick-keyboard", "picker-keyboard", "participants", "short-body", "picker-parent-menu", "menu-source-light", "menu-source-dark", "menu-peer-actions", "menu-own-actions", "menu-emoji-ink", "menu-encrypted-actions", "quick-capsule-ink", "menu-keyboard-reply", "menu-no-extra-star"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void visibleReactionsUseVerifiedOwnershipAndScopedVirtualPicker()
    {
        QFETCH(QString, scenario);
        const bool provider = scenario == "provider-remove" || scenario == "encrypted-metadata";
        const bool encrypted = scenario == "encrypted-metadata" || scenario == "menu-encrypted-actions";
        const QString providerName = "<b>future-provider</b>";
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        auto current = message((scenario == "quick-capsule" || scenario == "quick-keyboard" || scenario == "quick-capsule-ink") ? QString("Quick reaction capsule fixture\n").repeated(9) : QString("Reaction fixture body"));
        if (scenario == "menu-peer-actions" || scenario == "menu-keyboard-reply") {
            current["author_uuid"] = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
            current["is_own"] = false;
        }
        current["reactions"] = provider ? QJsonObject{{providerName, 2}} : QJsonObject{};
        current["reaction_users"] = QJsonObject{};
        if (scenario == "short-body") {
            current["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Hi"}};
            current["reactions"] = QJsonObject{{"heart", 2}};
        }
        if (scenario == "participants") {
            current["reactions"] = QJsonObject{{"heart", 2}};
            current["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId, returnedId}}};
        }
        const QString encryptedBody = "SYNTHETIC_ENCRYPTED_BODY_MUST_NOT_RENDER";
        if (encrypted) {
            const auto generation = returnedId;
            auto key = NativeChatCrypto::createGroupKey(); QVERIFY(key);
            const auto encoded = NativeChatCrypto::encryptMessage(key.bytes, encryptedBody.toUtf8(), {streamId, generation});
            key.bytes.fill(0); QVERIFY(encoded);
            current["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", generation},
                {"content", QString::fromLatin1(encoded.bytes.toBase64())}};
        }
        QString emoji = provider ? providerName : QString("thumbs_up");
        const auto fact = [&] {
            return QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
                {"user_uuid", userId}, {"emoji_name", emoji}, {"created_at", "2026-01-02T10:00:00Z"},
                {"updated_at", "2026-01-02T10:00:00Z"}};
        };
        QJsonArray ownFacts = provider ? QJsonArray{fact()} : QJsonArray{};
        int posts = 0, deletes = 0, ownReads = 0;
        bool validRequests = true;
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.contains("/message_reactions/")) {
                if (request.method == "GET") {
                    ++ownReads;
                    const QUrlQuery query(request.target);
                    validRequests = validRequests && query.queryItemValue("message_uuid") == messageId
                        && query.queryItemValue("user_uuid") == userId;
                    return Response{ownFacts};
                }
                if (request.method == "POST") {
                    ++posts;
                    validRequests = validRequests && request.body == QJsonObject{{"message_uuid", messageId}, {"emoji_name", "thumbs_up"}};
                    if (scenario == "uncertain-check") return Response{QJsonObject{}, 503};
                    ownFacts = QJsonArray{fact()};
                    current["reactions"] = QJsonObject{{emoji, 1}};
                    current["reaction_users"] = QJsonObject{{emoji, QJsonArray{userId}}};
                    return Response{fact(), 201, 180};
                }
                ++deletes;
                validRequests = validRequests && request.method == "DELETE" && path.endsWith("/" + returnedId);
                ownFacts = QJsonArray{};
                current["reactions"] = provider ? QJsonObject{{providerName, 1}} : QJsonObject{};
                current["reaction_users"] = QJsonObject{};
                return Response{QJsonObject{}, 204};
            }
            if (path.endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Reaction fixtures"}, {"encryption", encrypted}}}};
            if (path.endsWith("/users/")) return Response{scenario == "participants"
                ? QJsonArray{QJsonObject{{"uuid", userId}, {"username", "synthetic-participant"}, {"name", "<b>Verified</b> Owner"}, {"first_name", "<b>Verified</b>"}, {"last_name", "Owner"}}}
                : QJsonArray{}};
            if (path.endsWith("/stream_topics/")) {
                QJsonArray topics;
                for (const auto &id : {topicId, secondTopicId})
                    topics.append(QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", id == topicId ? "Reactions" : "Another topic"}});
                return Response{topics};
            }
            if (path.endsWith("/messages/"))
                return Response{QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId ? QJsonArray{} : QJsonArray{current}};
            if (path.endsWith("/messages/" + messageId)) return Response{current};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
            if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Window did not release focus before engine teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        controller.setRealtimeMode(true);
        click(window, item(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 2);
        click(window, item(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 1);
        auto *content = window->contentItem();
        QTRY_VERIFY(item(content, "message-" + messageId));
        auto *bubble = item(content, "message-" + messageId);
        if (scenario.startsWith("menu-")) {
            preferences.setMode(scenario == "menu-source-dark" ? "dark" : "light");
            QTest::mouseClick(window, Qt::RightButton, {}, bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
            auto *menu = window->findChild<QObject *>("messageMenu");
            QTRY_VERIFY(menu && menu->property("visible").toBool());
            auto *copy = item(content, "copyMessageAction");
            auto *reply = item(content, "replyMessageAction");
            auto *edit = item(content, "editMessageAction");
            auto *remove = item(content, "deleteMessageAction");
            QVERIFY(copy && reply && edit && remove);
            if (scenario == "menu-no-extra-star") {
                QVERIFY2(!window->findChild<QObject *>("starMessageAction"),
                    "Pinned source full caller has no Star action in the message context menu");
            } else if (scenario == "menu-keyboard-reply") {
                // End lands on Copy text; arrows skip separators and the
                // peer's hidden Edit/Delete rows. Source has no Star row.
                QTest::keyClick(window, Qt::Key_End);
                QTRY_VERIFY(copy->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Up);
                QTRY_VERIFY(reply->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_End);
                QTest::keyClick(window, Qt::Key_Home);
                QTRY_VERIFY(reply->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Return);
                QTRY_COMPARE(controller.draft(), QString("> Reaction fixture body\n\n"));
                QTRY_VERIFY(!menu->property("visible").toBool());
                QCOMPARE(posts + deletes, 0);
                const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
                return;
            } else if (scenario == "menu-encrypted-actions") {
                QVERIFY2(!reply->isVisible() && !copy->isVisible() && !edit->isVisible() && !remove->isVisible(),
                    "Locked message bodies must not expose unsupported Reply/Copy/Edit/Delete actions");
                QVERIFY(item(content, "moreReactionsButton")->isEnabled());
            } else if (scenario == "menu-peer-actions") {
                QVERIFY(!controller.messages().first().toMap().value("own").toBool());
                QVERIFY2(!edit->isVisible() && !remove->isVisible(), "Source hides Edit/Delete for peer messages");
                QVERIFY(!edit->isEnabled() && !remove->isEnabled());
            } else if (scenario == "menu-own-actions") {
                QVERIFY(edit->isVisible() && edit->isEnabled());
                QVERIFY(remove->isVisible() && remove->isEnabled());
                // Opening makes Menu visible before its scheduled row polish.
                // Assert the same actual source order after layout, not a timer.
                QTRY_VERIFY2(reply->mapToScene(QPointF()).y() < copy->mapToScene(QPointF()).y(), "Source Reply precedes Copy text");
                QTRY_VERIFY(copy->mapToScene(QPointF()).y() < edit->mapToScene(QPointF()).y());
                QTRY_VERIFY(edit->mapToScene(QPointF()).y() < remove->mapToScene(QPointF()).y());
            } else if (scenario == "menu-emoji-ink") {
#if !defined(Q_OS_LINUX)
                QSKIP("Record the matching OS source emoji ink before calibrating it.");
#endif
                auto *choice = item(content, "menu-reaction-thumbs_up");
                QVERIFY(choice && choice->isVisible());
                QTest::mouseMove(window, QPoint(2, 2));
                const auto image = window->grabWindow();
                const qreal scale = qreal(image.width()) / window->width();
                const auto point = choice->mapToScene(QPointF());
                const QRect cell(qRound(point.x() * scale), qRound(point.y() * scale),
                    qRound(choice->width() * scale), qRound(choice->height() * scale));
                QRect ink;
                for (int y = cell.top(); y <= cell.bottom(); ++y)
                    for (int x = cell.left(); x <= cell.right(); ++x) {
                        const auto color = image.pixelColor(x, y);
                        if (color.red() > 170 && color.green() > 100 && color.blue() < 130
                            && color.red() - color.green() < 160 && color.red() - color.blue() > 60)
                            ink |= QRect(x, y, 1, 1);
                    }
                QVERIFY(!ink.isEmpty());
                qInfo("Menu thumbs-up colored ink %.2fx%.2f DIP (source14x16)", ink.width() / scale, ink.height() / scale);
                QVERIFY(qAbs(ink.width() / scale - 14) <= 1);
                QVERIFY(qAbs(ink.height() / scale - 16) <= 1);
            } else {
                QCOMPARE(menu->property("width").toReal(), qreal(242));
                auto *bar = item(content, "reactionMenuBar");
                QVERIFY(bar);
                QCOMPARE(bar->height(), qreal(45));
                QCOMPARE(reply->height(), qreal(36));
                QCOMPARE(copy->height(), qreal(36));
                QCOMPARE(reply->property("text").toString(), QString("Reply"));
                QCOMPARE(copy->property("text").toString(), QString("Copy text"));
                QCOMPARE(QQmlProperty::read(reply, "icon.width").toReal(), qreal(20));
                QCOMPARE(QQmlProperty::read(copy, "icon.width").toReal(), qreal(20));
                QVERIFY(QQmlProperty::read(reply, "icon.source").toUrl().toString().endsWith("/reply.svg"));
                QVERIFY(QQmlProperty::read(copy, "icon.source").toUrl().toString().endsWith("/copy.svg"));
                auto *surface = item(content, "messageMenuSurface");
                QVERIFY(surface);
                const auto colors = window->property("colors").toMap();
                QCOMPARE(surface->property("color").value<QColor>(), QColor(colors.value("bg-elevated").toString()));
                QCOMPARE(surface->property("radius").toReal(), qreal(6));
                QCOMPARE(QQmlProperty::read(bar, "background.color").value<QColor>(), QColor(Qt::transparent));
                QVERIFY2(QQmlProperty::read(menu, "dim").toBool() == false, "Source context menu does not dim the scene");
            }
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!menu->property("visible").toBool());
            QCOMPARE(posts + deletes, 0);
            const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
            return;
        }
        if (scenario == "short-body") {
            QTRY_VERIFY(item(content, "reaction-" + messageId + "-heart"));
            auto *chip = item(content, "reaction-" + messageId + "-heart");
            auto *clock = item(content, "message-time-" + messageId);
            QVERIFY(clock);
            QVERIFY2(chip->width() >= 48, "Short text must not collapse the source reaction footer");
            const auto chipRight = chip->mapToItem(bubble, QPointF(chip->width(), 0)).x();
            QVERIFY(clock->x() - chipRight >= 7.5);
            QVERIFY(item(content, "reaction-count-" + messageId + "-heart")->isVisible());
            QCOMPARE(posts + deletes, 0);
            const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
            return;
        }
        if (scenario == "participants") {
            const auto prefix = "reaction-participant-name-" + messageId + "-heart-";
            QTRY_VERIFY(item(content, prefix + userId));
            QCOMPARE(item(content, prefix + userId)->property("text").toString(), QString("<"));
            QCOMPARE(item(content, prefix + userId)->property("textFormat").toInt(), 0);
            QCOMPARE(item(content, prefix + returnedId)->property("text").toString(), QString("#"));
            auto *chip = item(content, "reaction-" + messageId + "-heart");
            QVERIFY(chip);
            auto *accessible = QAccessible::queryAccessibleInterface(chip);
            QVERIFY(accessible);
            QVERIFY(accessible->text(QAccessible::Name).contains("<b>Verified</b> Owner, #77777777"));
            QTest::mouseMove(window, chip->mapToScene(QPointF(10, chip->height() / 2)).toPoint());
            QTRY_VERIFY(visibleItem("reactionTooltipText-" + messageId + "-heart"));
            auto *tooltipText = visibleItem("reactionTooltipText-" + messageId + "-heart");
            QCOMPARE(tooltipText->property("textFormat").toInt(), 0);
            QVERIFY(tooltipText->property("text").toString().contains("<b>Verified</b> Owner"));
            const QJsonObject renamed{{"uuid", userId}, {"username", "synthetic-participant"}, {"first_name", "Updated"}, {"last_name", "Owner"}, {"kind", "user.updated"}};
            QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
                {"uuid", "99000000-0000-4000-8000-000000000003"}, {"payload", renamed}}));
            QTRY_COMPARE(item(content, prefix + userId)->property("text").toString(), QString("U"));
            current["reactions"] = QJsonObject{{"heart", 3}};
            auto partial = current;
            partial["kind"] = "message.updated";
            QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
                {"uuid", "99000000-0000-4000-8000-000000000004"}, {"payload", partial}}));
            QTRY_VERIFY(!item(content, prefix + userId));
            auto *count = item(content, "reaction-count-" + messageId + "-heart");
            QVERIFY(count && count->isVisible());
            QCOMPARE(count->property("text").toString(), QString("3"));
            QCOMPARE(posts + deletes + ownReads, 0);
            const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
            return;
        }
        if (provider) {
            QTRY_VERIFY_WITH_TIMEOUT(item(content, "reaction-" + messageId + "-" + providerName), 1000);
            auto *chip = item(content, "reaction-" + messageId + "-" + providerName);
            QVERIFY(chip->isEnabled()); // Count-only metadata must check ownership.
            auto *label = item(content, "reaction-label-" + messageId + "-" + providerName);
            QVERIFY(label);
            QCOMPARE(label->property("text").toString(), providerName);
            QCOMPARE(label->property("textFormat").toInt(), 0);
            if (encrypted) {
                QVERIFY(!item(content, "message-content-" + messageId)->property("text").toString().contains(encryptedBody));
                auto *accessible = QAccessible::queryAccessibleInterface(bubble);
                QVERIFY(accessible && !accessible->text(QAccessible::Name).contains(encryptedBody));
                QVERIFY(!item(content, "messageComposer")->isEnabled());
            }
            click(window, chip);
            QTRY_COMPARE(deletes, 1);
            QTRY_VERIFY(!controller.messages().first().toMap().value("reactionBusy").toBool());
            QTRY_VERIFY(!item(content, "reaction-" + messageId + "-" + providerName)->isEnabled());
            QCOMPARE(posts, 0);
            QVERIFY(ownReads >= 2 && validRequests);
        } else {
            QTest::mouseMove(window, bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
            QTRY_VERIFY_WITH_TIMEOUT(item(content, "add-reaction-" + messageId), 1000);
            QTRY_VERIFY(item(content, "add-reaction-" + messageId)->isVisible());
            if (scenario == "quick-capsule" || scenario == "quick-keyboard" || scenario == "quick-capsule-ink") {
                if (scenario == "quick-keyboard") {
                    bubble->forceActiveFocus(Qt::TabFocusReason);
                    QTest::keyClick(window, Qt::Key_Tab);
                    auto *viewport = item(content, "quick-reactions-" + messageId);
                    QVERIFY(viewport);
                    QTRY_VERIFY(viewport->hasActiveFocus());
                    auto *capsule = item(content, "quickReactionCapsule-" + messageId);
                    QVERIFY(capsule);
                    QTRY_VERIFY(capsule->property("expanded").toBool());
                    QTest::keyClick(window, Qt::Key_Escape);
                    QTRY_VERIFY(!capsule->property("expanded").toBool());
                    QVERIFY(viewport->hasActiveFocus());
                    QTest::keyClick(window, Qt::Key_Down);
                    QTest::keyClick(window, Qt::Key_Down);
                    QTest::keyClick(window, Qt::Key_Return);
                    QTRY_COMPARE(posts, 1);
                    QTRY_VERIFY(!controller.messages().first().toMap().value("reactionBusy").toBool());
                    QVERIFY(validRequests);
                    const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
                    return;
                }
                auto *trigger = item(content, "add-reaction-" + messageId);
                QVERIFY(trigger);
                QTest::mouseMove(window, trigger->mapToScene(QPointF(trigger->width() / 2, trigger->height() / 2)).toPoint());
                QTRY_VERIFY_WITH_TIMEOUT(item(content, "quick-reaction-" + messageId + "-thumbs_up"), 1000);
                auto *second = item(content, "quick-reaction-" + messageId + "-thumbs_up");
                QVERIFY(second->isVisible());
                auto *viewport = item(content, "quick-reactions-" + messageId);
                QVERIFY(viewport);
                QTRY_COMPARE(viewport->property("contentHeight").toReal(), qreal(216));
                QTRY_VERIFY(second->mapToItem(viewport, QPointF()).y() >= 0);
                QTRY_VERIFY(second->mapToItem(viewport, QPointF()).y() + second->height() <= viewport->height());
                if (scenario == "quick-capsule-ink") {
#if !defined(Q_OS_LINUX)
                    QSKIP("Record the matching OS source emoji ink before calibrating it.");
#endif
                    const auto image = window->grabWindow();
                    const qreal scale = qreal(image.width()) / window->width();
                    const auto point = second->mapToScene(QPointF());
                    const QRect cell(qRound(point.x() * scale), qRound(point.y() * scale),
                        qRound(second->width() * scale), qRound(second->height() * scale));
                    QRect ink;
                    for (int y = cell.top(); y <= cell.bottom(); ++y)
                        for (int x = cell.left(); x <= cell.right(); ++x) {
                            const auto color = image.pixelColor(x, y);
                            if (color.red() > 170 && color.green() > 100 && color.blue() < 130
                                && color.red() - color.green() < 160 && color.red() - color.blue() > 60)
                                ink |= QRect(x, y, 1, 1);
                        }
                    QVERIFY(!ink.isEmpty());
                    qInfo("Corner capsule thumbs-up colored ink %.2fx%.2f DIP (source22x24)", ink.width() / scale, ink.height() / scale);
                    QVERIFY(qAbs(ink.width() / scale - 22) <= 1);
                    QVERIFY(qAbs(ink.height() / scale - 24) <= 1);
                    QCOMPARE(posts + deletes, 0);
                    const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
                    return;
                }
                click(window, second);
                QTRY_COMPARE(posts, 1);
                QTRY_VERIFY(!controller.messages().first().toMap().value("reactionBusy").toBool());
                QVERIFY(validRequests);
                const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
                return;
            }
            openFullReactionPicker(window, bubble);
            QTRY_VERIFY(item(content, "reactionGrid"));
            auto *grid = item(content, "reactionGrid");
            QTRY_COMPARE(grid->property("emojiCount").toInt(), 1895);
            if (scenario == "picker-parent-menu") {
                QVERIFY2(visibleItem("moreReactionsButton"), "Source context menu and quick choices remain visible beneath the full picker");
                QTest::keyClick(window, Qt::Key_Escape);
                QTRY_VERIFY(!grid->isVisible());
                const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
                return;
            }
            QTRY_VERIFY(itemCountWithPrefix(content, "emoji-choice-") > 0);
            QVERIFY(itemCountWithPrefix(content, "emoji-choice-") < 200);
            QTRY_VERIFY(item(content, "emoji-choice-grinning"));
            QTRY_VERIFY(item(content, "emoji-choice-grinning_face_with_big_eyes"));
            auto *firstChoice = item(content, "emoji-choice-grinning");
            auto *nextChoice = item(content, "emoji-choice-grinning_face_with_big_eyes");
            QCOMPARE(firstChoice->mapToScene(QPointF()).y(), nextChoice->mapToScene(QPointF()).y());
            QVERIFY(firstChoice->mapToScene(QPointF()).x() < nextChoice->mapToScene(QPointF()).x());
            if (scenario == "picker-categories-skins") {
                click(window, item(content, "reaction-category-flags"));
                QTRY_VERIFY(item(content, "emoji-choice-checkered_flag"));
                QVERIFY(itemCountWithPrefix(content, "emoji-choice-") < 200);
                click(window, item(content, "reactionSkinToneToggle"));
                click(window, item(content, "reaction-skin-1f3fd"));
                item(content, "reactionSearch")->setProperty("text", "waving hand");
                const auto waveName = canonicalReactionName(QString::fromUtf8("👋🏽"));
                QTRY_VERIFY(item(content, "emoji-choice-" + waveName));
                auto *wave = item(content, "emoji-choice-" + waveName);
                auto *glyph = wave->property("contentItem").value<QQuickItem *>();
                QVERIFY(glyph);
                QCOMPARE(glyph->property("text").toString(), QString::fromUtf8("👋🏽"));
                const auto center = wave->mapToScene(QPointF(20, 20)).toPoint();
                QTest::mousePress(window, Qt::LeftButton, {}, center);
                QTest::qWait(550);
                QTest::mouseRelease(window, Qt::LeftButton, {}, center);
                QTRY_VERIFY(visibleItem("emoji-variation-wave"));
                QCOMPARE(itemCountWithPrefix(content, "emoji-variation-"), 6);
                QCOMPARE(posts, 0);
                QTest::keyClick(window, Qt::Key_Escape);
                QTest::keyClick(window, Qt::Key_Escape);
                QTRY_VERIFY(!grid->isVisible());
                openFullReactionPicker(window, bubble);
                click(window, item(content, "reactionSkinToneToggle"));
                auto *neutral = item(content, "reaction-skin-neutral");
                QVERIFY(neutral);
                auto *accessible = QAccessible::queryAccessibleInterface(neutral);
                QVERIFY(accessible && accessible->state().checked);
                click(window, neutral);
                preferences.setLocale("ru");
                QCOMPARE(item(content, "reactionSearch")->property("placeholderText").toString(), QString("Search"));
                item(content, "reactionSearch")->setProperty("text", "no-source-emoji-matches-this-phrase");
                QTRY_COMPARE(grid->property("emojiCount").toInt(), 0);
                auto *searchResults = QAccessible::queryAccessibleInterface(grid);
                QVERIFY(searchResults);
                QCOMPARE(searchResults->text(QAccessible::Description), QString("No results found"));
                QCOMPARE(posts + deletes, 0);
                const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
                return;
            }
            if (scenario == "picker-fences") {
                preferences.setPaletteId("blue-cold");
                preferences.setMode("dark");
                const auto firstEmoji = QString("grinning");
                auto *choice = item(content, "emoji-choice-" + firstEmoji);
                QVERIFY(choice);
                auto *glyph = choice->property("contentItem").value<QQuickItem *>();
                QVERIFY(glyph);
                QTRY_COMPARE(glyph->property("color").value<QColor>(), QColor("#c0c0c0"));
                controller.selectTopic(secondTopicId);
                QTRY_VERIFY(!grid->isVisible());
                QVERIFY(QMetaObject::invokeMethod(window, "pickReaction", Q_ARG(QVariant, QString("thumbs_up"))));
                controller.selectTopic(topicId);
                QTRY_COMPARE(controller.messages().size(), 1);
                bubble = item(content, "message-" + messageId);
                QTest::mouseMove(window, bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
                QTRY_VERIFY(item(content, "add-reaction-" + messageId)->isVisible());
                openFullReactionPicker(window, bubble);
                QTRY_VERIFY(grid->isVisible());
                controller.logout();
                QTRY_VERIFY(!grid->isVisible());
                QVERIFY(QMetaObject::invokeMethod(window, "pickReaction", Q_ARG(QVariant, QString("thumbs_up"))));
                QTest::qWait(250);
                QCOMPARE(posts + deletes + ownReads, 0);
            } else {
                item(content, "reactionSearch")->setProperty("text", "thumbs_up");
                QTRY_VERIFY(item(content, "emoji-choice-thumbs_up"));
                if (scenario == "picker-keyboard") {
                    auto *search = item(content, "reactionSearch");
                    click(window, search);
                    QTest::keyClick(window, Qt::Key_Down);
                    QTRY_VERIFY(grid->hasActiveFocus());
                    QTest::keyClick(window, Qt::Key_Return);
                } else {
                    click(window, item(content, "emoji-choice-thumbs_up"));
                }
                QTRY_COMPARE(posts, 1);
                QTRY_VERIFY(!grid->isVisible());
                if (scenario == "uncertain-check") {
                    QTRY_VERIFY(controller.messages().first().toMap().value("reactionUncertain").toBool());
                    auto *chip = item(content, "reaction-" + messageId + "-thumbs_up");
                    QVERIFY(chip && !chip->isEnabled());
                    QTRY_VERIFY(item(content, "check-reaction-" + messageId));
                    ownFacts = QJsonArray{fact()};
                    current["reactions"] = QJsonObject{{emoji, 1}};
                    current["reaction_users"] = QJsonObject{{emoji, QJsonArray{userId}}};
                    click(window, item(content, "check-reaction-" + messageId));
                    QTRY_VERIFY(!controller.messages().first().toMap().value("reactionBusy").toBool());
                    QCOMPARE(posts, 1);
                } else {
                    QVERIFY(controller.messages().first().toMap().value("reactionBusy").toBool());
                    QTRY_VERIFY(!controller.messages().first().toMap().value("reactionBusy").toBool());
                }
                auto *chip = item(content, "reaction-" + messageId + "-thumbs_up");
                QVERIFY(chip && chip->isEnabled());
                QTRY_VERIFY(chip->property("owned").toBool());
                auto *accessible = QAccessible::queryAccessibleInterface(chip);
                QVERIFY(accessible && accessible->state().checkable && accessible->state().checked);
                if (scenario == "add-remove") {
                    click(window, chip);
                    QTRY_COMPARE(deletes, 1);
                    QTRY_VERIFY(!controller.messages().first().toMap().value("reactionBusy").toBool());
                    QTRY_VERIFY(!item(content, "reaction-" + messageId + "-thumbs_up"));
                }
                QVERIFY(validRequests);
            }
        }
        const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void inlineMetadataRequiresTheActualTrailingParagraph()
    {
        MockServer server;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        NativeMarkdown markdown;
        const QList<QPair<QString, bool>> cases{
            {"A **formatted** paragraph", true},
            {"First paragraph.\n\nFinal _paragraph_.", true},
            {"- First item\n- Last item", false},
            {"> Quoted paragraph", false},
            {"```cpp\nint value = 1;\n```", false},
            {"# Heading", false},
            {"| Left | Right |\n| --- | --- |\n| A | B |", false},
            {"- First item\n- Last item\n\nFinal paragraph.", true},
            {"[Hidden](urn:workspace:secret:not-a-public-reference)", true}
        };
        for (const auto &entry : cases) {
            const QVariant html = markdown.render(entry.first).value("html");
            QVariant result;
            QVERIFY(QMetaObject::invokeMethod(window, "trailingMessageParagraph",
                Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, html)));
            QVERIFY2((result.toInt() >= 0) == entry.second, qPrintable(entry.first + "\n" + html.toString()));
        }
        const QVariant safeHtml = markdown.render("**Safe**").value("html");
        const QVariant hostileClock = "<img src=\"https://invalid.example/clock\">&";
        QVariant reserved;
        QVERIFY(QMetaObject::invokeMethod(window, "reserveMessageTime", Q_RETURN_ARG(QVariant, reserved),
            Q_ARG(QVariant, safeHtml), Q_ARG(QVariant, hostileClock)));
        QVERIFY(reserved.toString().contains("&lt;img src=&quot;"));
        QVERIFY(!reserved.toString().contains("<img"));
        QVERIFY(reserved.toString().contains("&amp;"));
        window->close();
    }

    void sourcePickerGeometryAndHoverMatchPinnedComponent_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("geometry") << QString("geometry");
        QTest::newRow("hover") << QString("hover");
        QTest::newRow("title-font") << QString("title-font");
        QTest::newRow("glyph-ink") << QString("glyph-ink");
    }

    void sourcePickerGeometryAndHoverMatchPinnedComponent()
    {
        QFETCH(QString, scenario);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("testCatalog", knownReactionEmojis());
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import "qrc:/workspace/qml"
            NativeReactionPicker { width: 318; height: 358; choices: testCatalog; active: true }
        )QML", QUrl("qrc:/native-picker-source-geometry-test.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QQuickWindow window;
        QScopedPointer<QQuickItem> picker(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(picker);
        window.resize(360, 400);
        picker->setParentItem(window.contentItem());
        picker->setPosition(QPointF(21, 21)); // Source outer border is a separate 1px inset.
        window.show();
        window.requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *search = item(picker.get(), "reactionSearch");
        QTRY_VERIFY(item(picker.get(), "emoji-choice-grinning"));
        auto *first = item(picker.get(), "emoji-choice-grinning");
        auto *second = item(picker.get(), "emoji-choice-grinning_face_with_big_eyes");
        QVERIFY(search && first && second);
        if (scenario == "geometry") {
            QCOMPARE(search->mapToItem(picker.get(), QPointF()), QPointF(11, 16));
            QCOMPARE(search->size(), QSizeF(268, 40));
            QCOMPARE(first->mapToItem(picker.get(), QPointF()), QPointF(11, 171));
            QCOMPARE(first->size(), QSizeF(40, 40));
            QCOMPARE(second->mapToScene(QPointF()).x() - first->mapToScene(QPointF()).x(), qreal(40));
        } else if (scenario == "title-font") {
            auto *heading = item(picker.get(), "reaction-heading-smileys_people");
            QVERIFY(heading);
            const auto font = heading->property("font").value<QFont>();
            QFont sourceSans("Arial");
            QCOMPARE(QFontInfo(font).family(), QFontInfo(sourceSans).family());
            QCOMPARE(font.pixelSize(), 16);
            QCOMPARE(font.weight(), QFont::Bold);
        } else if (scenario == "glyph-ink") {
#if !defined(Q_OS_LINUX)
            QSKIP("The recorded colored-ink reference is Linux; record the matching OS reference first.");
#endif
            const auto image = window.grabWindow();
            QVERIFY(!image.isNull());
            const qreal scale = qreal(image.width()) / window.width();
            const auto point = first->mapToScene(QPointF());
            const QRect cell(qRound(point.x() * scale), qRound(point.y() * scale),
                qRound(first->width() * scale), qRound(first->height() * scale));
            QRect ink;
            for (int y = cell.top(); y <= cell.bottom(); ++y)
                for (int x = cell.left(); x <= cell.right(); ++x) {
                    const auto color = image.pixelColor(x, y);
                    if (color.red() > 170 && color.green() > 100 && color.blue() < 130
                        && color.red() - color.green() < 160 && color.red() - color.blue() > 60)
                        ink |= QRect(x, y, 1, 1);
                }
            QVERIFY(!ink.isEmpty());
            qInfo("First emoji colored ink %.2fx%.2f DIP (source32x31)", ink.width() / scale, ink.height() / scale);
            QVERIFY(qAbs(ink.width() / scale - 32) <= 1);
            QVERIFY(qAbs(ink.height() / scale - 31) <= 1);
        } else {
            QTest::mouseMove(&window, first->mapToScene(QPointF(20, 20)).toPoint());
            QTest::qWait(800); // Allow the existing hover tooltip to appear if still present.
            QVERIFY2(!hasVisibleText(window.contentItem(), "grinning"), "Pinned source has title=null and preview disabled; emoji hover adds no tooltip");
            auto *accessible = QAccessible::queryAccessibleInterface(first);
            QVERIFY(accessible);
            QCOMPARE(accessible->text(QAccessible::Name), QString("grinning face"));
        }
        window.close();
        picker->setParentItem(nullptr);
    }

    void sourcePickerSuggestedHistoryPersistsAndClearsBetweenOwners()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString ownerA(64, QLatin1Char('a')), ownerB(64, QLatin1Char('b'));
        NativePreferences preferences(path);
        preferences.setOwner(ownerA);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("testPreferences", &preferences);
        engine.rootContext()->setContextProperty("testCatalog", knownReactionEmojis());
        engine.rootContext()->setContextProperty("testOwner", ownerA);
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import "qrc:/workspace/qml"
            NativeReactionPicker {
                width: 320; height: 360
                choices: testCatalog
                active: true
                suggested: testPreferences.suggestedReactions
                onChosen: name => testPreferences.recordSuggestedReaction(name, testOwner)
            }
        )QML", QUrl("qrc:/native-picker-history-test.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QQuickWindow window;
        QScopedPointer<QQuickItem> picker(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(picker);
        window.resize(360, 400);
        picker->setParentItem(window.contentItem());
        picker->setPosition(QPointF(20, 20));
        window.show();
        window.requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *search = item(picker.get(), "reactionSearch");
        auto *grid = item(picker.get(), "reactionGrid");
        QVERIFY(search && grid);
        QCOMPARE(grid->property("emojiCount").toInt(), 1895);
        for (const auto &name : {QString("wave"), QString("blue_heart"), QString("wave")}) {
            search->setProperty("text", name);
            QTRY_VERIFY(item(picker.get(), "emoji-choice-" + name));
            click(&window, item(picker.get(), "emoji-choice-" + name));
        }
        QCOMPARE(preferences.suggestedReactions().size(), 2);
        QCOMPARE(preferences.suggestedReactions().first().toMap().value("name").toString(), QString("wave"));
        QCOMPARE(preferences.suggestedReactions().first().toMap().value("count").toInt(), 2);
        QVERIFY(QMetaObject::invokeMethod(picker.get(), "reset"));
        QTRY_COMPARE(grid->property("emojiCount").toInt(), 1897);
        QTRY_VERIFY(item(picker.get(), "emoji-choice-wave"));
        QTRY_VERIFY(item(picker.get(), "emoji-choice-blue_heart"));
        auto *wave = item(picker.get(), "emoji-choice-wave");
        auto *blueHeart = item(picker.get(), "emoji-choice-blue_heart");
        QVERIFY(wave->mapToItem(grid, QPointF()).x() < blueHeart->mapToItem(grid, QPointF()).x());
        QVERIFY(itemCountWithPrefix(picker.get(), "emoji-choice-") < 200);
        NativePreferences restored(path);
        restored.setOwner(ownerA);
        QCOMPARE(restored.suggestedReactions(), preferences.suggestedReactions());
        preferences.setOwner(ownerB);
        QTRY_COMPARE(grid->property("emojiCount").toInt(), 1895);
        QVERIFY(preferences.suggestedReactions().isEmpty());
        // A late click captured under A must not populate B's suggestions.
        preferences.recordSuggestedReaction("heart", ownerA);
        QVERIFY(preferences.suggestedReactions().isEmpty());
        preferences.setOwner(ownerA);
        QTRY_COMPARE(grid->property("emojiCount").toInt(), 1897);
        window.close();
        picker->setParentItem(nullptr);
    }

    void onlyTheActiveVisibleViewportAdvancesTheReadBoundary()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        QJsonArray history;
        for (int i = 0; i < 40; ++i) {
            auto entry = message(QString("Visible boundary fixture %1").arg(i),
                QString("80000000-0000-4000-8000-%1").arg(i, 12, 10, QLatin1Char('0')));
            entry["is_own"] = false;
            entry["read"] = false;
            entry["created_at"] = QDateTime::fromString("2026-01-02T10:00:00.000Z", Qt::ISODateWithMs)
                .addSecs(i).toString(Qt::ISODateWithMs);
            history.append(entry);
        }
        QStringList anchors;
        const auto unread = [&history] {
            int count = 0;
            for (const auto &entry : history) count += !entry.toObject().value("read").toBool();
            return count;
        };
        const auto stream = [&] {
            return QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Read state"},
                {"encryption", false}, {"unread_count", unread()}, {"active_unread_count", unread()},
                {"passive_unread_count", 0}, {"last_message_uuid", history.last().toObject().value("uuid")}};
        };
        const auto topic = [&] {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Viewport"}, {"unread_count", unread()}, {"active_unread_count", unread()},
                {"passive_unread_count", 0}, {"last_message_uuid", history.last().toObject().value("uuid")}};
        };
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return Response{QJsonArray{stream()}};
            if (path.endsWith("/streams/" + streamId)) return Response{stream()};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) return Response{QJsonArray{topic()}};
            if (path.endsWith("/stream_topics/" + topicId)) return Response{topic()};
            if (path.endsWith("/messages/")) return Response{history};
            if (path.endsWith("/actions/read_up_to/invoke")) {
                const auto id = path.section('/', -4, -4);
                int boundary = -1;
                for (int i = 0; i < history.size(); ++i)
                    if (history[i].toObject().value("uuid") == id) boundary = i;
                if (boundary < 0) return Response{QJsonObject{}, 404};
                anchors.append(id);
                for (int i = 0; i <= boundary; ++i) {
                    auto entry = history[i].toObject();
                    entry["read"] = true;
                    history[i] = entry;
                }
                return Response{history[boundary]};
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.setRealtimeMode(true);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        controller.reloadSnapshot();
        QTRY_COMPARE(controller.streams().size(), 1);
        // Both stream and topic now load real history. Keep the other window
        // active before either selection, so this checks inactive loading.
        // Load the history while a separate real native window owns focus.
        QQuickWindow other;
        other.setTitle("Synthetic focus target");
        other.resize(300, 200);
        other.show();
        other.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1);
        controller.selectTopic(topicId);
        QTRY_COMPARE(controller.messages().size(), 40);
        auto *list = item(window->contentItem(), "messageList");
        QVERIFY(list);
        // Model delivery precedes the initial measured tail placement. The
        // user-visible history must be positioned before deliberately moving it.
        QTRY_VERIFY(!list->property("positioning").toBool());
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        // An absence assertion spans the actual 250 ms batching interval.
        QTest::qWait(400);
        QVERIFY(anchors.isEmpty());
        QVERIFY(!window->isActive());

        const auto lowestVisible = [&] {
            QString anchor;
            for (const auto &entry : controller.messages()) {
                const auto id = entry.toMap().value("id").toString();
                auto *bubble = item(window->contentItem(), "message-" + id);
                if (!bubble || !bubble->isVisible()) continue;
                const auto point = bubble->mapToItem(list, QPointF());
                if (point.y() < list->height() && point.y() + bubble->height() > 0
                    && point.x() < list->width() && point.x() + bubble->width() > 0)
                    anchor = id;
            }
            return anchor;
        };
        QTRY_VERIFY(!lowestVisible().isEmpty());
        const auto firstAnchor = lowestVisible();
        QVERIFY(firstAnchor != controller.messages().last().toMap().value("id").toString());
        other.hide();
        window->raise();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_COMPARE_WITH_TIMEOUT(anchors.size(), 1, 1500);
        QCOMPARE(anchors.first(), firstAnchor);
        QTRY_VERIFY(controller.messages().first().toMap().value("read").toBool());
        QVERIFY(!controller.messages().last().toMap().value("read").toBool());
        QTRY_COMPARE(controller.topics().first().toMap().value("activeUnread").toInt(), unread());
        QTest::qWait(400);
        QCOMPARE(anchors.size(), 1);
        QCOMPARE(lowestVisible(), firstAnchor);
        QVERIFY(!controller.messages().last().toMap().value("read").toBool());

        other.show();
        other.raise();
        other.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTest::qWait(400);
        QCOMPARE(anchors.size(), 1);
        window->showMinimized();
        QTRY_COMPARE(window->visibility(), QWindow::Minimized);
        QTest::qWait(400);
        QCOMPARE(anchors.size(), 1);
        window->hide();
        QTest::qWait(400);
        QCOMPARE(anchors.size(), 1);
        other.hide();
        window->setWindowStates(Qt::WindowNoState);
        window->showNormal();
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->raise();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_COMPARE_WITH_TIMEOUT(anchors.size(), 2, 1500);
        QCOMPARE(anchors.last(), controller.messages().last().toMap().value("id").toString());
        QTRY_COMPARE(controller.topics().first().toMap().value("unread").toInt(), 0);
        QTest::qWait(400);
        QCOMPARE(anchors.size(), 2);
        openChannelNewChat(window);
        QTRY_VERIFY(visibleItem("channelNameInput"));
        auto incoming = message("An unread arrival remains covered by New chat", returnedId);
        incoming["author_uuid"] = channelPeer;
        incoming["is_own"] = false;
        incoming["read"] = false;
        incoming["created_at"] = "2026-01-02T11:00:00.000Z";
        history.append(incoming);
        incoming["kind"] = "message.created";
        QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
            {"uuid", "84000000-0000-4000-8000-000000000003"}, {"payload", incoming}}));
        QTRY_COMPARE(controller.messages().size(), 41);
        QTest::qWait(400);
        QCOMPARE(anchors.size(), 2);
        QVERIFY(!controller.messages().last().toMap().value("read").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!visibleItem("channelNameInput"));
        QTRY_COMPARE_WITH_TIMEOUT(anchors.size(), 3, 1500);
        QCOMPARE(anchors.last(), returnedId);
        other.close();
        window->close();
    }

    void windowTeardownWaitsForVisibleIncubationBeforeHide()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("incubation.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto closeWindow=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Incubation witness window did not close",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        auto *incubation=engine.incubationController();QVERIFY(incubation);
        QCOMPARE(incubation,window->incubationController());QTRY_COMPARE(incubation->incubatingObjectCount(),0);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nItem { objectName: \"pendingWindowTeardownItem\"; width: 1; height: 1 }",QUrl("inline:window-teardown-item.qml"));
        QTRY_VERIFY2(component.isReady(),qPrintable(component.errorString()));
        QQmlIncubator pending(QQmlIncubator::Asynchronous);
        pending.setInitialProperties({{"parent",QVariant::fromValue(window->contentItem())}});
        const auto clearPending=qScopeGuard([&pending] {
            const QPointer<QObject> created(pending.object());
            pending.clear(); // Only this test-owned fixture, including failed RED setup.
            if(created)delete created.data();
        });
        // An observer of the real public Hide event, not an injected lifecycle.
        // It cannot drive incubation, change input or suppress warnings.
        struct HideProbe final:QObject {
            QQmlEngine &engine;int countAtFirstHide=-1;
            explicit HideProbe(QQmlEngine &value):engine(value){}
            bool eventFilter(QObject *,QEvent *event) override {
                if(event->type()==QEvent::Hide&&countAtFirstHide<0) {
                    const auto *control=engine.incubationController();
                    countAtFirstHide=control?control->incubatingObjectCount():-1;
                }
                return false;
            }
        } probe(engine);
        window->installEventFilter(&probe);
        component.create(pending);
        // Positive admission BEFORE any event pump/close. If this setup is not
        // actually asynchronous, stop; no lifecycle RED may be claimed.
        QVERIFY(pending.isLoading());QCOMPARE(incubation->incubatingObjectCount(),1);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)closeWindow.dismiss();QVERIFY(closed);
        QCOMPARE(probe.countAtFirstHide,0);
        QCOMPARE(incubation->incubatingObjectCount(),0);QVERIFY(pending.isReady());
    }

    void messageUpdatesRespectTheVisibleScrollPosition_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("incoming-away") << QString("incoming-away");
        QTest::newRow("own-incoming-away") << QString("own-incoming-away");
        QTest::newRow("prepend") << QString("prepend");
        QTest::newRow("prepend-day-boundary") << QString("prepend-day-boundary");
        QTest::newRow("incoming-bottom") << QString("incoming-bottom");
        QTest::newRow("own-send-away") << QString("own-send-away");
        for (const auto *scenario : {"incoming-away", "own-incoming-away", "prepend-day-boundary", "incoming-bottom"})
            QTest::newRow(qPrintable("aggregate-" + QString::fromLatin1(scenario)))
                << "aggregate-" + QString::fromLatin1(scenario);
    }

    void messageUpdatesRespectTheVisibleScrollPosition()
    {
        QFETCH(QString, scenario);
        const bool aggregate = scenario.startsWith("aggregate-");
        const QString behavior = aggregate ? scenario.mid(10) : scenario;
        const bool prepend = behavior.startsWith("prepend");
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        QMap<int, QJsonObject> history;
        const auto row = [&](int number, bool own = false) {
            auto entry = message(QString("Scroll fixture %1%2").arg(number)
                    .arg(QString("\nVariable-height message").repeated(number % 3)),
                QString("81000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0')),
                aggregate && number % 2 ? secondTopicId : topicId);
            entry["author_uuid"] = own ? userId : QString("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
            entry["is_own"] = own;
            const auto date = behavior == "prepend-day-boundary" && number < 30
                ? "2026-01-01T10:00:00.000Z" : "2026-01-02T10:00:00.000Z";
            entry["created_at"] = QDateTime::fromString(date, Qt::ISODateWithMs)
                .addSecs(number).toString(Qt::ISODateWithMs);
            return entry;
        };
        for (int i = 0; i < 80; ++i) history.insert(i, row(i));
        QStringList readAnchors;
        QJsonObject sent;
        const auto unread = [&] {
            int count = 0;
            for (const auto &entry : history) count += !entry.value("read").toBool();
            return count;
        };
        const auto stream = [&] {
            return QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Scroll fixture"},
                {"encryption", false}, {"unread_count", unread()}, {"active_unread_count", unread()},
                {"passive_unread_count", 0}, {"last_message_uuid", history.last().value("uuid")}};
        };
        const auto topic = [&](const QString &id = topicId) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Viewport preservation"}, {"unread_count", unread()}, {"active_unread_count", unread()},
                {"passive_unread_count", 0}, {"last_message_uuid", history.last().value("uuid")}};
        };
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return Response{QJsonArray{stream()}};
            if (path.endsWith("/streams/" + streamId)) return Response{stream()};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) return aggregate
                ? Response{QJsonArray{topic(), topic(secondTopicId)}} : Response{QJsonArray{topic()}};
            if (path.endsWith("/stream_topics/" + topicId)) return Response{topic()};
            if (aggregate && path.endsWith("/stream_topics/" + secondTopicId)) return Response{topic(secondTopicId)};
            if (path.endsWith("/messages/") && request.method == "GET") {
                const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
                const auto boundary = marker.isEmpty() ? 1000 : marker.right(12).toInt();
                QJsonArray page;
                bool more = false;
                for (auto it = history.cend(); it != history.cbegin();) {
                    --it;
                    if (it.key() >= boundary) continue;
                    if (page.size() == 50) { more = true; break; }
                    page.append(it.value());
                }
                Response response{page};
                if (more) response.headers.insert("X-Pagination-Marker",
                    page.last().toObject().value("uuid").toString().toUtf8());
                return response;
            }
            if (path.endsWith("/messages/") && request.method == "POST") {
                sent = row(81, true);
                sent["read"] = false;
                sent["payload"] = request.body.value("payload");
                history.insert(81, sent);
                return Response{sent};
            }
            if (path.endsWith("/actions/read_up_to/invoke")) {
                const auto id = path.section('/', -4, -4);
                auto boundary = std::find_if(history.begin(), history.end(), [&id](const auto &entry) {
                    return entry.value("uuid") == id;
                });
                if (boundary == history.end()) return Response{QJsonObject{}, 404};
                readAnchors.append(id);
                for (auto it = history.begin(); it.key() <= boundary.key(); ++it) {
                    if (it.value()["topic_uuid"] == boundary.value()["topic_uuid"])
                        it.value()["read"] = true;
                    if (it == boundary) break;
                }
                return Response{boundary.value()};
            }
            if (!sent.isEmpty() && path.endsWith("/messages/" + sent.value("uuid").toString()))
                return Response{sent};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Scroll window did not settle creation before teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        controller.setRealtimeMode(true);
        click(window, item(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), aggregate ? 2 : 1);
        if (!aggregate)
            click(window, item(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 50);
        auto *list = item(window->contentItem(), "messageList");
        QVERIFY(list);
        const auto lastId = controller.messages().last().toMap().value("id").toString();
        QTRY_VERIFY(item(window->contentItem(), "message-" + lastId));
        QTRY_VERIFY(item(window->contentItem(), "message-" + lastId)->mapToItem(list, QPointF()).y() < list->height());
        QTRY_VERIFY(list->property("atYEnd").toBool());
        const auto settleWheel = [&] {
            QElapsedTimer stable;
            stable.start();
            auto previousY = list->property("contentY").toReal();
            return QTest::qWaitFor([&] {
                const auto currentY = list->property("contentY").toReal();
                if (qAbs(currentY - previousY) > 0.01) {
                    previousY = currentY;
                    stable.restart();
                }
                // Wheel animation can outlive Flickable.moving. Observe its
                // actual coordinate settling before capturing the user's view.
                return !list->property("moving").toBool() && stable.elapsed() >= 50;
            }, 1500);
        };
        const auto firstVisible = [&] {
            QPair<QString, qreal> anchor;
            for (const auto &entry : controller.messages()) {
                const auto id = entry.toMap().value("id").toString();
                auto *bubble = item(window->contentItem(), "message-" + id);
                if (!bubble || !bubble->isVisible()) continue;
                const auto point = bubble->mapToItem(list, QPointF());
                if (point.y() < list->height() && point.y() + bubble->height() > 0) {
                    anchor = {id, point.y()};
                    break;
                }
            }
            return anchor;
        };
        if (behavior != "incoming-bottom") {
            const auto before = list->property("contentY").toReal();
            const auto center = list->mapToScene(QPointF(list->width() / 2, list->height() / 2));
            QTest::mouseMove(window, center.toPoint());
            QTest::wheelEvent(window, center, QPoint(0, prepend ? 120000 : 1200));
            QTRY_VERIFY(list->property("contentY").toReal() < before - 100);
            QVERIFY(settleWheel());
            const auto distanceFromBottom = [&] {
                return list->property("originY").toReal() + list->property("contentHeight").toReal()
                    + list->property("bottomMargin").toReal() - list->property("contentY").toReal() - list->height();
            };
            // A wheel event is capped by Qt. Move clearly beyond the source's
            // 80px tail-follow threshold before asserting history preservation.
            for (int steps = 0; steps < 10 && distanceFromBottom() < 500; ++steps) {
                QTest::wheelEvent(window, center, QPoint(0, 1200));
                QVERIFY(settleWheel());
            }
            QVERIFY(distanceFromBottom() >= 500);
            if (prepend) {
                QTRY_VERIFY(list->property("atYBeginning").toBool());
                QTest::wheelEvent(window, center, QPoint(0, -120));
                QVERIFY(settleWheel());
            }
        }
        QTRY_VERIFY(!list->property("positioning").toBool());
        const auto anchor = firstVisible();
        QVERIFY(!anchor.first.isEmpty());
        if (!aggregate) {
            QQuickItem *visibleAvatar = nullptr;
            int avatarCount = 0;
            for (const auto &value : controller.messages()) {
                auto *avatar = item(window->contentItem(), "message-avatar-" + value.toMap()["id"].toString());
                if (avatar && avatar->isVisible()) { visibleAvatar = avatar; ++avatarCount; }
            }
            QCOMPARE(avatarCount, 1);
            QVERIFY(visibleAvatar);
            const auto point = visibleAvatar->mapToItem(list, QPointF());
            QVERIFY(point.y() >= 0);
            QVERIFY(point.y() + visibleAvatar->height() <= list->height() - 7);
            if (!list->property("atYEnd").toBool())
                QVERIFY(qAbs(point.y() + visibleAvatar->height() - list->height() + 8) <= 1);
            const auto instantiated = itemCountWithPrefix(window->contentItem(), "message-avatar-")
                - itemCountWithPrefix(window->contentItem(), "message-avatar-initial-");
            QVERIFY(instantiated < controller.messages().size());
        }
        const auto oldY = list->property("contentY").toReal();
        const auto oldOriginY = list->property("originY").toReal();
        if (prepend) {
            QVERIFY(controller.canLoadOlder());
            click(window, dialogButton(window->contentItem(), "Load older messages"));
            QTRY_COMPARE(controller.messages().size(), 80);
        } else if (behavior == "own-send-away") {
            item(window->contentItem(), "messageComposer")->setProperty("text", "A locally sent row pins to the tail.");
            QTRY_VERIFY(controller.canSend());
            click(window, item(window->contentItem(), "sendButton"));
            QTRY_COMPARE(controller.messages().size(), 51);
            QTRY_VERIFY(controller.draft().isEmpty());
        } else {
            auto incoming = row(80, behavior == "own-incoming-away");
            incoming["read"] = false;
            history.insert(80, incoming);
            incoming["kind"] = "message.created";
            QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
                {"uuid", "99999999-9999-4999-8999-000000000080"}, {"payload", incoming}}));
            QTRY_COMPARE(controller.messages().size(), 51);
        }
        // Also spans the real visible-read batching interval: an accidental
        // jump must not silently mark the unseen incoming tail read.
        QTest::qWait(400);
        if (behavior == "incoming-away" || behavior == "own-incoming-away" || prepend) {
            if (prepend) {
                // Source computeWorkspaceScrollTopFromAnchor preserves the old
                // UUID and offset. Moving the first day's divider can expose a
                // newly prepended row in its former leading space; that does
                // not move the preserved message or make its UUID disappear.
                auto *preserved = item(window->contentItem(), "message-" + anchor.first);
                QVERIFY(preserved && preserved->isVisible());
                const auto position = preserved->mapToItem(list, QPointF()).y();
                QVERIFY(position < list->height() && position + preserved->height() > 0);
                QVERIFY2(qAbs(position - anchor.second) <= 1,
                    qPrintable(QString("Preserved prepend anchor %1 moved %2 -> %3")
                        .arg(anchor.first).arg(anchor.second).arg(position)));
            } else {
                QCOMPARE(firstVisible().first, anchor.first);
                QVERIFY(qAbs(firstVisible().second - anchor.second) <= 1);
                const auto newY = list->property("contentY").toReal();
                const auto newOriginY = list->property("originY").toReal();
                QVERIFY2(qAbs(newY - oldY) <= 1,
                    qPrintable(QString("Scroll offset changed: y %1 -> %2, origin %3 -> %4")
                        .arg(oldY).arg(newY).arg(oldOriginY).arg(newOriginY)));
                QVERIFY(!controller.messages().last().toMap().value("read").toBool());
            }
            QVERIFY(readAnchors.isEmpty());
        } else {
            QTRY_VERIFY(list->property("atYEnd").toBool());
            QTRY_COMPARE(readAnchors.size(), 1);
            QCOMPARE(readAnchors.last(), controller.messages().last().toMap().value("id").toString());
            QTRY_VERIFY(controller.messages().last().toMap().value("read").toBool());
        }
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)closeBeforeEngine.dismiss();QVERIFY(closed);
    }

    void resolvedTopicReopensThroughTheVisibleMenu_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *value : {"mouse", "keyboard", "unconfirmed"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }

    void resolvedTopicReopensThroughTheVisibleMenu()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        MockServer server;
        bool done = false, valid = true;
        int toggles = 0, confirmations = 0, sends = 0;
        const auto topic = [&] {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Reopen fixture"}, {"is_done", done}};
        };
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return Response{QJsonArray{QJsonObject{{"uuid", streamId},
                {"project_id", projectId}, {"name", "Reopen fixture"}, {"encryption", false}}}};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) return Response{QJsonArray{topic()}};
            if (path.endsWith("/actions/toggle_done/invoke")) {
                ++toggles;
                valid = valid && request.method == "POST" && request.body.isEmpty();
                if (scenario != "unconfirmed") done = false;
                return Response{QJsonObject{}, scenario == "unconfirmed" ? 503 : 200, 120};
            }
            if (path.endsWith("/stream_topics/" + topicId)) {
                ++confirmations;
                return Response{topic(), scenario == "unconfirmed" ? 503 : 200, 120};
            }
            if (path.endsWith("/messages/")) {
                if (request.method != "GET") ++sends;
                return Response{QJsonArray{message("Resolved history remains visible")}};
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        controller.setRealtimeMode(true);
        click(window, item(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, item(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 1);
        auto *composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer);
        composer->setProperty("text", "Draft survives a verified reopen");
        done = true;
        auto closed = topic();closed["kind"] = "topic.updated";
        QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
            {"uuid", "99000000-0000-4000-8000-000000000003"}, {"payload", closed}}));
        QTRY_VERIFY(controller.topicClosed());
        QTRY_VERIFY(controller.canReopenTopic());
        QVERIFY(QFile::exists(":/workspace/icons/check.svg"));
        QTRY_VERIFY(item(window->contentItem(), "reopenTopicButton"));
        auto *trigger = item(window->contentItem(), "reopenTopicButton");
        QVERIFY(trigger->isVisible() && trigger->isEnabled());
        auto *accessible = QAccessible::queryAccessibleInterface(trigger);
        QVERIFY(accessible && accessible->role() == QAccessible::Button);
        QCOMPARE(accessible->text(QAccessible::Name), QString("Remove the mark"));
        if (scenario == "keyboard") {
            trigger->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Space);
        } else click(window, trigger);
        QTRY_VERIFY(visibleItem("reopenTopicAction"));
        QCOMPARE(toggles, 0); // Opening the source menu is not the mutation.
        auto *action = visibleItem("reopenTopicAction");
        if (scenario == "keyboard") {
            action->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Return);
        } else click(window, action);
        QTRY_COMPARE(toggles, 1);
        QVERIFY(!controller.canReopenTopic());
        QVERIFY(!trigger->isEnabled());
        QVERIFY(!composer->isVisible());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(confirmations, 1);
        if (scenario == "unconfirmed") {
            QTRY_VERIFY(!controller.error().isEmpty());
            QVERIFY(hasVisibleText(window->contentItem(), controller.error()));
            QVERIFY(controller.topicClosed() && !controller.canReopenTopic());
            QVERIFY(!composer->isVisible() && !composer->isEnabled());
        } else {
            QTRY_VERIFY(!controller.topicClosed());
            QTRY_VERIFY(composer->isVisible() && composer->isEnabled());
            QCOMPARE(composer->property("text").toString(), QString("Draft survives a verified reopen"));
        }
        QCOMPARE(controller.draft(), QString("Draft survives a verified reopen"));
        QCOMPARE(toggles, 1);
        QCOMPARE(sends, 0);
        QVERIFY(valid);
        window->close();
    }

    void closedTopicPromptPreservesTheDraftThroughRealtimeReopen()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        MockServer server;
        const auto topic = [](bool done) {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Resolved topic"}, {"is_done", done}};
        };
        int sends = 0;
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return Response{QJsonArray{QJsonObject{{"uuid", streamId},
                {"project_id", projectId}, {"name", "Closed topic fixture"}, {"encryption", false}}}};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) return Response{QJsonArray{topic(false)}};
            if (path.endsWith("/messages/")) {
                if (request.method != "GET") ++sends;
                return Response{QJsonArray{message("A resolved conversation keeps its history")}};
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        FileClient files(&api);
        controller.useFiles(&files);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        controller.setRealtimeMode(true);
        click(window, item(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, item(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 1);
        auto *composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isEnabled());
        composer->setProperty("text", "Keep this unsent draft after topic resolution");
        QTRY_VERIFY(controller.canSend());
        auto close = topic(true);
        close["kind"] = "topic.updated";
        QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
            {"uuid", "99000000-0000-4000-8000-000000000001"}, {"payload", close}}));
        QTRY_VERIFY(controller.topicClosed());
        QVERIFY(!controller.canSend() && !controller.canAttach());
        QVERIFY(!composer->isEnabled());
        QVERIFY(!composer->isVisible());
        QTRY_VERIFY(item(window->contentItem(), "topicClosedPrompt"));
        QVERIFY(item(window->contentItem(), "topicClosedPrompt")->isVisible());
        QCOMPARE(controller.draft(), QString("Keep this unsent draft after topic resolution"));
        auto reopen = topic(false);
        reopen["kind"] = "topic.updated";
        QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
            {"uuid", "99000000-0000-4000-8000-000000000002"}, {"payload", reopen}}));
        QTRY_VERIFY(!controller.topicClosed());
        QTRY_VERIFY(composer->isVisible() && composer->isEnabled());
        QVERIFY(!item(window->contentItem(), "topicClosedPrompt")->isVisible());
        QCOMPARE(composer->property("text").toString(), controller.draft());
        QCOMPARE(sends, 0);
        window->close();
    }

    void sessionWarningDoesNotCancelVisibleSnapshotRecovery()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        server.handler = [](const Request &request) {
            if (request.target.path().endsWith("/streams/") || request.target.path().endsWith("/users/")) {
                Response response{QJsonArray{}};
                response.delay = 50;
                return response;
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.setRealtimeMode(true);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        QSignalSpy failed(&controller, &NativeController::snapshotFailed);
        QSignalSpy recovered(&controller, &NativeController::snapshotReloaded);
        controller.reloadSnapshot();
        QVERIFY(controller.busy());
        // A keychain warning arrives through the same Session signal after a
        // verified sign-in. It must leave the in-flight catalog barrier intact.
        session.error("Synthetic secure credential store unavailable");
        QCOMPARE(failed.size(), 0);
        QTRY_COMPARE(recovered.size(), 1);
        QVERIFY(controller.authenticated());
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(!controller.busy());
        window->close();
    }

    void rememberedAccountResumesAndRemovesThroughVisibleQuickControls()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        WindowSecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        SavedAccount saved{{}, QUrl("https://workspace.example"), projectId, userId,
                           "synthetic-user", "Synthetic project"};
        saved.id = SessionVault::accountId(saved.server, saved.project, saved.user);
        bool stored = false;
        vault.save(saved, "synthetic-refresh-private", [&](QString error) {
            QVERIFY(error.isEmpty()); stored = true;
        });
        QTRY_VERIFY(stored);
        ApiClient api;
        int requests = 0;
        Session session(&api, nullptr,
            [&](const QByteArray &method, const QString &path, const QJsonValue &body,
                const QUrlQuery &, ApiClient::Callback done, bool,
                const QMap<QByteArray, QByteArray> &) {
            ++requests;
            Request request{method, QUrl(path), {}, body.toObject()};
            auto response = sessionResponse(request);
            if (path.endsWith("/get_token/invoke")) {
                QCOMPARE(body.toObject().value("grant_type").toString(), QString("refresh_token"));
                auto data = response.data.toObject();
                data.insert("refresh_token", "synthetic-rotated-private");
                response.data = data;
            }
            ApiResult result{response.data, response.status, {}, {}};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        controller.setRealtimeMode(true);
        QCOMPARE(controller.savedAccounts().size(), 1);
        const auto metadata = controller.savedAccounts().first().toMap();
        QCOMPARE(metadata.keys(), (QStringList{"id", "login", "project", "projectName", "server", "user"}));
        QVERIFY(!QJsonDocument(QJsonObject::fromVariantMap(metadata)).toJson().contains("synthetic-refresh-private"));
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *content = window->contentItem();
        QTRY_VERIFY_WITH_TIMEOUT(item(content, "resume-" + saved.id), 1000);
        QCOMPARE(item(content, "remembered-label-" + saved.id)->property("text").toString(), saved.projectName);
        backend.holdRead = true;
        backend.readError = SessionSecretStore::Error::Unavailable;
        click(window, item(content, "resume-" + saved.id));
        QVERIFY(controller.busy());
        QVERIFY(!item(content, "resume-" + saved.id)->isEnabled());
        QVERIFY(!item(content, "forget-" + saved.id)->isEnabled());
        QVERIFY(!item(content, "passwordInput")->isEnabled());
        backend.finishRead();
        QTRY_VERIFY(!controller.error().isEmpty());
        QVERIFY(!controller.authenticated());
        QCOMPARE(controller.savedAccounts().size(), 1);
        QCOMPARE(requests, 0);
        backend.readError = SessionSecretStore::Error::None;
        click(window, item(content, "resume-" + saved.id));
        backend.finishRead();
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QVERIFY(controller.error().isEmpty());
        QCOMPARE(requests, 2);
        QTRY_COMPARE(backend.values.value(saved.id), QByteArray("synthetic-rotated-private"));
        QVERIFY(item(content, "passwordInput")->property("text").toString().isEmpty());
        QVERIFY(!item(content, "messageComposer")->isEnabled());
        click(window, item(content, "accountMenuButton"));
        QTRY_VERIFY(item(content, "signOutButton")->isVisible());
        click(window, item(content, "signOutButton"));
        QTRY_VERIFY(!controller.authenticated());
        QTRY_VERIFY(controller.savedAccounts().isEmpty());
        QTRY_VERIFY(backend.values.isEmpty());
        stored = false;
        vault.save(saved, "synthetic-remove-private", [&](QString error) {
            QVERIFY(error.isEmpty()); stored = true;
        });
        QTRY_VERIFY(stored);
        QTRY_VERIFY(item(content, "forget-" + saved.id));
        click(window, item(content, "forget-" + saved.id));
        QTRY_VERIFY(controller.savedAccounts().isEmpty());
        QTRY_VERIFY(backend.values.isEmpty());
        window->close();
    }

    void sourceConversationInsetsAndAvatarFallback_data()
    {
        QTest::addColumn<QString>("check");
        QTest::newRow("container-origin") << QString("origin");
        QTest::newRow("avatar-light") << QString("light");
        QTest::newRow("avatar-dark") << QString("dark");
        QTest::newRow("topbar-full-frame") << QString("topbar");
        QTest::newRow("sidebar-search-full-frame") << QString("search");
        QTest::newRow("sidebar-topic-full-frame") << QString("topic");
        QTest::newRow("date-section-full-frame") << QString("date");
        QTest::newRow("date-grouping-localization") << QString("date-groups");
        QTest::newRow("folder-rail-full-frame") << QString("rail");
        QTest::newRow("header-no-extra-call") << QString("header-actions");
        QTest::newRow("composer-full-frame") << QString("composer");
        QTest::newRow("sidebar-stream-content-inset") << QString("stream-inset");
    }

    void sourceConversationInsetsAndAvatarFallback()
    {
        QFETCH(QString, check);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setPaletteId("blue-mist");
        preferences.setMode(check == "dark" ? "dark" : "light");
        MockServer server;
        server.handler = [](const Request &request) {
            if (request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Source layout fixture"}, {"description", "Not a message preview"}, {"encryption", false}}}};
            if (request.target.path().endsWith("/users/")) return Response{QJsonArray{}};
            if (request.target.path().endsWith("/stream_topics/"))
                return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Source topic"}}}};
            if (request.method == "GET" && request.target.path().endsWith("/messages/")) {
                auto peer = message("Source fallback avatar and inset");
                peer.insert("author_uuid", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
                peer.insert("is_own", false);
                return Response{QJsonArray{peer}};
            }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *content = window->contentItem();
        QTRY_COMPARE(controller.streams().size(), 1);
        controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1);
        controller.selectTopic(topicId);
        QTRY_COMPARE(controller.messages().size(), 1);
        auto *list = item(content, "messageList");
        QTRY_VERIFY(!list->property("positioning").toBool());
        if (check == "topbar") {
            auto *header = item(content, "conversationHeader");
            QVERIFY(header);
            // Actual pinned full-frame TopBar: toolbar48 + p-2*2 + border1,
            // then mb-1. macOS also reserves its separate28px titlebar strip.
            QCOMPARE(header->mapToScene(QPointF()).y(), qreal(69 + (QOperatingSystemVersion::currentType() == QOperatingSystemVersion::MacOS ? 28 : 0)));
        } else if (check == "composer") {
            auto *send = item(content, "sendButton");
            QVERIFY(send && send->parentItem() && send->parentItem()->parentItem());
            QCOMPARE(send->parentItem()->parentItem()->height(), qreal(57));
            QCOMPARE(send->width(), qreal(40));
            QCOMPARE(send->height(), qreal(40));
        } else if (check == "rail") {
            auto *rail = item(content, "folderRail");
            auto *all = item(content, "folder-all");
            auto *personal = item(content, "folder-personal");
            QVERIFY(rail && all && personal);
            // Source72px item/p4, selected64px inner scaled1.1, icon top10.
            QVERIFY(qAbs(all->mapToItem(rail, QPointF()).y() - 19.8) < 0.1);
            QCOMPARE(personal->mapToItem(rail, QPointF()).y(), qreal(98));
        } else if (check == "header-actions") {
            auto *header = item(content, "conversationHeader");
            QVERIFY(header);
            for (auto *child : header->findChildren<QQuickItem *>())
                QVERIFY2(child->property("asset").toString() != "phone.svg" || !child->isVisible(),
                    "Source hides the call affordance when no verified call integration is configured");
        } else if (check == "search") {
            auto *sidebar = item(content, "sidebar");
            auto *search = item(content, "sidebarSearch");
            QVERIFY(sidebar && search);
            QCOMPARE(search->property("placeholderText").toString(), QString("Find"));
            QCOMPARE(search->height(), qreal(32));
            QCOMPARE(search->x(), qreal(12));
            QCOMPARE(search->y(), qreal(16));
            QCOMPARE(search->width(), sidebar->width() - 64);
            QCOMPARE(search->property("leftPadding").toReal(), qreal(9));
            QCOMPARE(search->property("topPadding").toReal(), qreal(3));
            auto *create = item(content, "createChatButton");
            auto *activity = item(content, "sidebarActivity");
            QVERIFY(create && activity);
            QVERIFY(create->isEnabled()); // Ordinary channel creation now owns this source entry.
            QCOMPARE(create->width(), qreal(32));
            QCOMPARE(activity->y(), qreal(64));
            QCOMPARE(activity->height(), qreal(32));
            QCOMPARE(item(content, "stream-" + streamId)->mapToItem(sidebar, QPointF()).y(), qreal(104));
            QVERIFY(!hasVisibleText(sidebar, "Not a message preview"));
        } else if (check == "stream-inset") {
            auto *stream = item(content, "stream-" + streamId);
            QVERIFY(stream);
            auto *body = stream->property("contentItem").value<QQuickItem *>();
            QVERIFY(body);
            QCOMPARE(body->x(), qreal(10));
            QCOMPARE(body->y(), qreal(10));
        } else if (check == "topic") {
            auto *topic = item(content, "topic-" + topicId);
            QVERIFY(topic);
            QCOMPARE(topic->height(), qreal(56));
            auto *body = topic->property("contentItem").value<QQuickItem *>();
            QVERIFY(body);
            QCOMPARE(body->x(), qreal(0));
            QCOMPARE(body->y(), qreal(0));
        } else if (check == "date" || check == "date-groups") {
            // The dedicated HTTP contract validates createdAt projection; this
            // view seam also runs against the frozen pre-projection core archive.
            auto rows = controller.messages();
            auto first = rows.first().toMap();
            first.insert("createdAt", "2026-01-02T10:00:00.000Z");
            rows[0] = first;
            if (check == "date-groups") {
                auto sameDay = first;
                sameDay.insert("id", returnedId);
                sameDay.insert("createdAt", "2026-01-02T10:01:00.000Z");
                rows.append(sameDay);
                auto nextDay = first;
                nextDay.insert("id", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
                nextDay.insert("createdAt", "2026-01-03T10:00:00.000Z");
                rows.append(nextDay);
            }
            QVERIFY(QMetaObject::invokeMethod(list, "replaceMessages", Q_ARG(QVariant, QVariant(rows)), Q_ARG(QVariant, QVariant(false))));
            QTRY_VERIFY(!list->property("positioning").toBool());
            QTRY_VERIFY(item(content, "messageDay-2026-01-02"));
            auto *divider = item(content, "messageDay-2026-01-02");
            QCOMPARE(divider->height(), qreal(26));
            QCOMPARE(divider->property("text").toString(), QString("January 2"));
            auto *bubble = item(content, "message-" + messageId);
            QCOMPARE(bubble->mapToItem(list, QPointF()).y(), qreal(58));
            if (check == "date-groups") {
                QTRY_VERIFY(item(content, "messageDay-2026-01-03"));
                auto *nextDivider = item(content, "messageDay-2026-01-03");
                auto *sameDay = item(content, "message-" + returnedId);
                auto *nextDay = item(content, "message-bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
                QVERIFY(sameDay && nextDay);
                // Same author/topic/day stays in one source group: 4px inside,
                // 8px only between groups. A new day still adds its section.
                QCOMPARE(sameDay->mapToItem(list, QPointF()).y() - bubble->mapToItem(list, QPointF(0, bubble->height())).y(), qreal(4));
                QCOMPARE(nextDay->mapToItem(list, QPointF()).y() - sameDay->mapToItem(list, QPointF(0, sameDay->height())).y(), qreal(50));
                QCOMPARE(nextDivider->property("text").toString(), QString("January 3"));
                preferences.setLocale("ru");
                QTRY_COMPARE(divider->property("text").toString(), QString::fromUtf8("2 января"));
                QTRY_COMPARE(nextDivider->property("text").toString(), QString::fromUtf8("3 января"));
            }
        } else if (check == "origin") {
            auto *header = item(content, "conversationHeader");
            auto *bubble = item(content, "message-" + messageId);
            QVERIFY(header && bubble);
            // Source ChatPageWorkspace header/section flex-col has no gap;
            // WorkspaceMessageList contributes its own py-4 (16 px) inset.
            QCOMPARE(list->mapToScene(QPointF()).y(), header->mapToScene(QPointF(0, header->height())).y());
            const bool dated = !controller.messages().first().toMap().value("createdAt").toString().isEmpty();
            QCOMPARE(bubble->mapToScene(QPointF()).y() - list->mapToScene(QPointF()).y(), qreal(dated ? 58 : 16));
        } else {
            auto *initial = item(content, "message-avatar-initial-" + messageId);
            QVERIFY(initial);
            // Shared Avatar's generated text-text-primary utility wins the
            // equal-specificity text-accent-soft class in the pinned stylesheet.
            const auto colors = window->property("colors").toMap();
            QCOMPARE(initial->property("color").value<QColor>(), QColor(colors.value("text-primary").toString()));
            const auto font = initial->property("font").value<QFont>();
            QCOMPARE(font.pixelSize(), 18);
            QCOMPARE(font.weight(), QFont::DemiBold);
        }
        window->close();
    }

    void cacheSettingsSeparateRequestedFromAppliedViewStates()
    {
        // View-state seam only; the adjacent appearance test covers the actual
        // NativeCacheService production binding and physical10->24GB policy.
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        MockServer server;server.handler = sessionResponse;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        preferences.setLocale("en");
        preferences.setCacheLimitGb(24);
        preferences.reportCacheStatus(true, false, 10000000000LL, 125000000LL);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
            if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Window did not release focus before engine teardown",__FILE__,__LINE__);
        });
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *content = window->contentItem();
        click(window, item(content, "accountMenuButton"));
        QTRY_VERIFY(item(content, "cacheSettingsLimits"));
        auto *limits = item(content, "cacheSettingsLimits");
        auto *busy = item(content, "cacheSettingsBusy");
        auto *used = item(content, "cacheSettingsUsage");
        auto *unavailable = item(content, "cacheSettingsUnavailable");
        QVERIFY(busy && used && unavailable);
        QVERIFY(busy->isVisible());
        QCOMPARE(limits->property("text").toString(), QString("Requested: 24 GB · Applied: 10 GB"));
        auto *limit = item(content, "cacheLimitGb");
        QVERIFY(limit && limit->isEnabled());
        auto *field = limit->property("contentItem").value<QQuickItem *>();
        click(window, field);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_1);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_COMPARE(preferences.cacheLimitGb(), 1);
        QCOMPARE(limits->property("text").toString(), QString("Requested: 1 GB · Applied: 10 GB"));
        preferences.reportCacheStatus(false, true, 1000000000LL, 125000000LL);
        QVERIFY(!busy->isVisible());
        QCOMPARE(limits->property("text").toString(), QString("Requested: 1 GB · Applied: 1 GB"));
        QVERIFY(used->isVisible());
        QCOMPARE(used->property("text").toString(), QString("Used: %1").arg(
            QLocale("en").formattedDataSize(125000000LL, 1, QLocale::DataSizeSIFormat)));
        preferences.reportCacheStatus(true, false, 0, 0);
        QCOMPARE(limits->property("text").toString(), QString("Requested: 1 GB"));
        QVERIFY(!used->isVisible());
        preferences.reportCacheStatus(false, false, 0, 0);
        QVERIFY(unavailable->isVisible());
        controller.logout();
        QVERIFY(!unavailable->isVisible()); // No false unavailable label without an owner.
        const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void appearanceAndSidebarRestoreAfterQuickWindowRecreation()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        MockServer server;
        server.handler = sessionResponse;
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.setRealtimeMode(true);
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_VERIFY(session.ready());
        qreal savedWidth = 0;
        QColor savedColor;
        {
            NativePreferences preferences(path);
            NativeCacheService cache(directory.filePath("cache"), {}, true);
            bindNativeCachePreferences(cache, preferences);
            QVERIFY(cache.open(server.origin(), projectId, userId));
            QTRY_VERIFY(cache.available());
            QTRY_COMPARE(cache.appliedDiskLimitBytes(), qint64(10000000000LL));
            QTRY_COMPARE(preferences.cacheAppliedLimitBytes(), qint64(10000000000LL));
            QQmlApplicationEngine engine;
            loadNativeUi(engine, &controller, &preferences);
            QVERIFY(!engine.rootObjects().isEmpty());
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            QVERIFY(window);
            auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
                if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Window did not release focus before engine teardown",__FILE__,__LINE__);
            });
            QVERIFY(QTest::qWaitForWindowExposed(window));
            auto *content = window->contentItem();
            click(window, item(content, "accountMenuButton"));
            QTRY_VERIFY(item(content, "appearance-mode-dark"));
            click(window, item(content, "appearance-mode-dark"));
            QCOMPARE(preferences.mode(), QString("dark"));
            click(window, item(content, "palette-emerald-chat"));
            QCOMPARE(preferences.paletteId(), QString("emerald-chat"));
            click(window, item(content, "languageSetting"));
            click(window, item(content, "language-ru"));
            QCOMPARE(preferences.locale(), QString("ru"));
            QTRY_COMPARE(item(content, "refreshButton")->property("text").toString(), QString::fromUtf8("Обновить"));
            click(window, item(content, "language-en"));
            QCOMPARE(preferences.locale(), QString("en"));
            QTRY_COMPARE(item(content, "refreshButton")->property("text").toString(), QString("Refresh"));
            auto *limit = item(content, "cacheLimitGb");
            QVERIFY(limit);
            QCOMPARE(limit->property("value").toInt(), 10);
            auto *field = limit->property("contentItem").value<QQuickItem *>();
            QVERIFY(field);
            click(window, field);
            QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClick(window, Qt::Key_2);
            QTest::keyClick(window, Qt::Key_4);
            QTest::keyClick(window, Qt::Key_Tab);
            QTRY_COMPARE(preferences.cacheLimitGb(), 24);
            QTRY_COMPARE(cache.appliedDiskLimitBytes(), qint64(24000000000LL));
            QTRY_COMPARE(preferences.cacheAppliedLimitBytes(), qint64(24000000000LL));
            QTRY_VERIFY(!preferences.cacheBusy());
            preferences.reportCacheError("<b>Synthetic cache failure</b>");
            QTRY_VERIFY(item(content, "cacheSettingsError")->isVisible());
            QCOMPARE(item(content, "cacheSettingsError")->property("text").toString(), QString("<b>Synthetic cache failure</b>"));
            QCOMPARE(item(content, "cacheSettingsError")->property("textFormat").toInt(), 0);
            preferences.reportCacheError({});
            QTRY_VERIFY(!item(content, "cacheSettingsError")->isVisible());
            QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(1100, 500));
            auto *handle = item(content, "sidebarResizeHandle");
            QVERIFY(handle);
            const auto origin = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
            QTest::mousePress(window, Qt::LeftButton, {}, origin);
            QTest::mouseMove(window, origin + QPoint(64, 0));
            QTest::mouseRelease(window, Qt::LeftButton, {}, origin + QPoint(64, 0));
            QTRY_VERIFY(preferences.sidebarPreferredWidth() > 299);
            savedWidth = preferences.sidebarPreferredWidth();
            savedColor = window->color();
            const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
        }
        NativePreferences restored(path);
        NativeCacheService restoredCache(directory.filePath("cache"), {}, true);
        bindNativeCachePreferences(restoredCache, restored);
        QVERIFY(restoredCache.open(server.origin(), projectId, userId));
        QTRY_VERIFY(restoredCache.available());
        QTRY_COMPARE(restoredCache.appliedDiskLimitBytes(), qint64(24000000000LL));
        QTRY_COMPARE(restored.cacheAppliedLimitBytes(), qint64(24000000000LL));
        QCOMPARE(restored.mode(), QString("dark"));
        QCOMPARE(restored.cacheLimitGb(), 24);
        QCOMPARE(restored.paletteId(), QString("emerald-chat"));
        QCOMPARE(restored.sidebarPreferredWidth(), savedWidth);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &restored);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([guard=QPointer<QQuickWindow>(window)] {
            if(guard && !closeObservedTestWindow(guard.data())) QTest::qFail("Window did not release focus before engine teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(window->color(), savedColor);
        QCOMPARE(window->property("preferredSidebarWidth").toDouble(), savedWidth);
        auto *content = window->contentItem();
        click(window, item(content, "accountMenuButton"));
        QTRY_VERIFY(item(content, "appearance-mode-system"));
        click(window, item(content, "appearance-mode-system"));
        QCOMPARE(restored.mode(), QString("system"));
        const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void quickFileDialogsUploadThenSaveAnAuthenticatedAttachment()
    {
        QTemporaryDir directory;
        const auto source = directory.filePath("synthetic.bin");
        const auto destination = directory.filePath("saved.bin");
        {
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("{}"), 2);
        }
        const QJsonObject metadata{{"uuid", returnedId}, {"stream_uuid", streamId},
            {"name", "synthetic.bin"}, {"content_type", QMimeDatabase().mimeTypeForFile(source, QMimeDatabase::MatchContent).name()},
            {"size_bytes", 2}, {"hash", QString::fromLatin1(QCryptographicHash::hash("{}", QCryptographicHash::Sha256).toHex())}};
        MockServer server;
        int uploads = 0, downloads = 0;
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Synthetic channel"}, {"encryption", false}}}};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/"))
                return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Attachments"}}}};
            if (path.endsWith("/messages/"))
                return Response{QJsonArray{message("[Download synthetic attachment](urn:file:" + returnedId + ')')}};
            if (path.endsWith("/files/") && request.method == "POST") { ++uploads; return Response{metadata, 201}; }
            if (path.endsWith("/files/" + returnedId)) return Response{metadata};
            if (path.endsWith("/actions/download")) { ++downloads; return Response{QJsonObject{}}; }
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        FileClient files(&api);
        NativeController controller(&api, &session);
        controller.useFiles(&files);
        NativePreferences preferences(directory.filePath("preferences.ini"));
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *content = window->contentItem();
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        click(window, item(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, item(content, "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 1);
        QVERIFY(controller.canAttach());
        auto *picker = window->findChild<QObject *>("attachmentPicker");
        auto *saveDialog = window->findChild<QObject *>("attachmentSaveDialog");
        QVERIFY(picker && saveDialog);
        QVERIFY(picker->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        click(window, item(content, "attachButton"));
        QTRY_VERIFY(picker->property("visible").toBool());
        QCOMPARE(window->property("pickerOwner").toString(), controller.ownerKey());
        QCOMPARE(window->property("pickerStream").toString(), controller.selectedStream());
        QCOMPARE(window->property("pickerTopic").toString(), controller.selectedTopic());
        QVERIFY(controller.canAttach());
        QTRY_VERIFY(fileEntry(QUrl::fromLocalFile(source)));
        auto *entry = fileEntry(QUrl::fromLocalFile(source));
        QSignalSpy accepted(picker, SIGNAL(accepted()));
        const auto point = entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2)).toPoint();
        QTest::mouseDClick(entry->window(), Qt::LeftButton, {}, point);
        QTRY_COMPARE(accepted.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(uploads == 1 || !controller.error().isEmpty(), 2000);
        QVERIFY2(uploads == 1, qPrintable(controller.error()));
        QTRY_VERIFY(controller.draft().contains("urn:file:" + returnedId) || !controller.error().isEmpty());
        QVERIFY2(controller.draft().contains("urn:file:" + returnedId), qPrintable(controller.error()));
        QTRY_VERIFY(!controller.fileBusy());
        QVERIFY(!controller.notice().isEmpty());
        QCOMPARE(item(content, "fileTransferNotice")->property("text").toString(), controller.notice());
        QVERIFY(saveDialog->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        click(window, item(content, "message-content-" + messageId));
        QTRY_VERIFY(saveDialog->property("visible").toBool());
        QTRY_VERIFY(visibleItem("fileNameTextField"));
        auto *filename = visibleItem("fileNameTextField");
        click(filename->window(), filename);
        QTest::keySequence(filename->window(), QKeySequence::SelectAll);
        for (const auto character : QByteArray("saved.bin"))
            QTest::keyClick(filename->window(), character);
        QCOMPARE(filename->property("text").toString(), QString("saved.bin"));
        QTRY_VERIFY(dialogButton("Save"));
        auto *saveButton = dialogButton("Save");
        QSignalSpy saveAccepted(saveDialog, SIGNAL(accepted()));
        click(saveButton->window(), saveButton);
        QTRY_COMPARE(saveAccepted.size(), 1);
        QTRY_VERIFY(downloads == 1 || !controller.error().isEmpty());
        QVERIFY2(downloads == 1, qPrintable(controller.error()));
        QTRY_VERIFY(!controller.fileBusy());
        QFile saved(destination);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("{}"));
        QVERIFY(controller.notice().contains("saved.bin"));
        click(window, item(content, "attachButton"));
        QTRY_VERIFY(picker->property("visible").toBool());
        controller.selectTopic({});
        QTRY_VERIFY(!picker->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(picker, "accepted"));
        QCoreApplication::processEvents();
        QCOMPARE(uploads, 1);
        window->close();
    }

    void maskedSecretFragmentNeverAppearsInAccessibleMessageName()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        MockServer server;
        server.handler = [](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Synthetic channel"}, {"encryption", false}}}};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/"))
                return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Synthetic topic"}}}};
            if (path.endsWith("/messages/"))
                return Response{QJsonArray{message("[SYNTHETIC PRIVATE LABEL](urn:secret:v99:synthetic-envelope)")}};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1);
        controller.selectTopic(topicId);
        QTRY_COMPARE(controller.messages().size(), 1);
        auto *content = window->contentItem();
        QTRY_VERIFY(item(content, "message-" + messageId));
        auto *bubble = item(content, "message-" + messageId);
        QAccessible::setActive(true);
        auto *accessible = QAccessible::queryAccessibleInterface(bubble);
        QVERIFY(accessible);
        const auto name = accessible->text(QAccessible::Name);
        QVERIFY(!name.contains("SYNTHETIC PRIVATE LABEL"));
        QVERIFY(!name.contains("synthetic-envelope"));
        QVERIFY(!name.contains("urn:secret:"));
        QVERIFY(name.contains("Encrypted fragment"));
        window->close();
    }
};

class NativeCacheWindowWorker final : public QObject {
    Q_OBJECT
private slots:
    void cachePhase()
    {
        QTest::failOnWarning(QRegularExpression(".*(?:TypeError|ReferenceError):.*"));
        QTest::failOnWarning(QRegularExpression(".*Binding loop detected.*"));
        const bool restore = qEnvironmentVariable("CASSI_NATIVE_CACHE_CHILD_PHASE") == "restore";
        const QString directory = qEnvironmentVariable("CASSI_NATIVE_CACHE_CHILD_DIRECTORY");
        const QUrl origin(qEnvironmentVariable("CASSI_NATIVE_CACHE_CHILD_ORIGIN"));
        QVERIFY(!directory.isEmpty() && origin.host() == "127.0.0.1");
        NativeCacheService cache(directory, {}, true);
        NativePreferences preferences(QDir(directory).filePath("appearance.ini"));
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.useCache(&cache);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        auto *content = window->contentItem();
        session.login(origin, "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        click(window, item(content, "project-" + projectId));
        QTRY_COMPARE(controller.streams().size(), 1);
        preferences.setMode("light");
        click(window, item(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, item(content, "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 2);
        QTRY_VERIFY(item(content, "message-content-" + messageId));
        QVERIFY(item(content, "message-content-" + messageId)->property("text").toString().contains("Saved message before restart"));
        if (restore) {
            QVERIFY(controller.cacheStale());
            auto *list = item(content, "messageList");
            QTRY_VERIFY(!list->property("positioning").toBool());
            auto *firstBubble = item(content, "message-" + messageId);
            const bool dated = !controller.messages().first().toMap().value("createdAt").toString().isEmpty();
            QCOMPARE(firstBubble->mapToItem(list, QPointF()).y(), list->property("topMargin").toReal() + (dated ? 42 : 0));
            auto *composer = item(content, "messageComposer");
            composer->setProperty("text", "Draft while saved history is being checked");
            QVERIFY(!item(content, "sendButton")->isEnabled());
            QVERIFY(!item(content, "attachButton")->isEnabled());
            auto *bubble = item(content, "message-" + messageId);
            const auto point = bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2));
            QTest::mouseClick(window, Qt::RightButton, {}, point.toPoint());
            QVERIFY(!window->findChild<QObject *>("starMessageAction"));
            for (const auto *name : {"editMessageAction", "deleteMessageAction"}) {
                auto *action = window->findChild<QObject *>(name);
                QVERIFY(action);
                QVERIFY2(!action->property("enabled").toBool(), name);
            }
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(item(content, "cacheStatus"));
            auto *status = item(content, "cacheStatus");
            QVERIFY(status->isVisible());
            QVERIFY(status->property("text").toString().contains("Updating"));
            QTRY_VERIFY_WITH_TIMEOUT(!status->property("truncated").toBool(), 1000);
            auto *activity = item(content, "cacheUpdatingIndicator");
            QVERIFY(activity && activity->isVisible());
            auto *spinner = activity->property("contentItem").value<QQuickItem *>();
            QVERIFY(spinner);
            QCOMPARE(spinner->width(), 14);
            QCOMPARE(spinner->height(), 14);
            const auto updatingScreenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_CACHE_SCREENSHOT");
            if (!updatingScreenshot.isEmpty()) {
                QTest::mouseMove(window, QPoint(1100, 500));
                QVERIFY(window->grabWindow().save(updatingScreenshot));
            }
            QTest::qWait(350);
            QVERIFY(controller.cacheStale());
            QTRY_VERIFY(!controller.busy());
            QVERIFY(status->isVisible());
            QVERIFY(status->property("text").toString().contains("Update failed"));
            QTRY_VERIFY_WITH_TIMEOUT(!status->property("truncated").toBool(), 1000);
            const auto offlineScreenshot = qEnvironmentVariable("CASSI_NATIVE_TEST_CACHE_OFFLINE_SCREENSHOT");
            if (!offlineScreenshot.isEmpty()) {
                QTest::mouseMove(window, QPoint(1100, 500));
                QVERIFY(window->grabWindow().save(offlineScreenshot));
            }
            QVERIFY(!list->property("readActive").toBool());
            QTRY_VERIFY(item(content, "cacheRetryButton"));
            click(window, item(content, "cacheRetryButton"));
            QVERIFY(controller.busy());
            QVERIFY(status->property("text").toString().contains("Updating"));
            QTRY_VERIFY(!controller.busy());
            QVERIFY(!controller.cacheStale());
            QVERIFY(!status->isVisible());
            QCOMPARE(controller.messages().size(), 1);
            QTRY_VERIFY(!item(content, "message-" + returnedId));
            QTRY_VERIFY(item(content, "message-content-" + messageId)->property("text").toString().contains("Verified updated message after reconnect"));
            QVERIFY(item(content, "sendButton")->isEnabled());
            QTRY_VERIFY(item(content, "reaction-" + messageId + "-heart")->isEnabled());
            QCOMPARE(controller.draft(), QString("Draft while saved history is being checked"));

            bubble = item(content, "message-" + messageId);
            QTest::mouseClick(window, Qt::RightButton, {}, bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
            auto *editAction = qobject_cast<QQuickItem *>(window->findChild<QObject *>("editMessageAction"));
            QVERIFY(editAction && editAction->isEnabled());
            click(window, editAction);
            auto *dialog = window->findChild<QObject *>("editMessageDialog");
            QVERIFY(dialog);
            QTRY_VERIFY(dialog->property("visible").toBool());
            QTRY_VERIFY(window->findChild<QObject *>("saveMessageEditButton"));
            auto *saveEdit = window->findChild<QObject *>("saveMessageEditButton");
            auto *edit = item(content, "editMessageInput");
            QVERIFY(edit);
            edit->setProperty("text", "Uncommitted edit during connection recovery");
            controller.refresh();
            QVERIFY(controller.busy());
            QVERIFY(dialog->property("visible").toBool());
            QCOMPARE(edit->property("text").toString(), QString("Uncommitted edit during connection recovery"));
            QVERIFY(!saveEdit->property("enabled").toBool());
            QTRY_VERIFY(!controller.busy());
            QVERIFY(dialog->property("visible").toBool());
            QVERIFY(saveEdit->property("enabled").toBool());
            QCOMPARE(edit->property("text").toString(), QString("Uncommitted edit during connection recovery"));
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!dialog->property("visible").toBool());

            // A new identity must not see the previous account's persisted rows
            // while its own delayed catalog requests are still pending.
            session.login(origin, "second-synthetic-user", "synthetic-password");
            QTRY_COMPARE(session.projects().size(), 1);
            click(window, item(content, "project-" + projectId));
            QVERIFY(controller.messages().isEmpty());
            QVERIFY(controller.streams().isEmpty());
            QVERIFY(!item(content, "message-" + messageId));
            QVERIFY(!controller.cacheStale());
            QTRY_COMPARE(session.user(), QString("88888888-8888-4888-8888-888888888888"));
        }
        window->close();
        cache.shutdown();
        QTRY_VERIFY(cache.stopped());
    }
};

int main(int argc, char **argv)
{
    // Exercise Qt's visible fallback dialog deterministically; platform-native
    // picker/portal acceptance remains part of each OS's manual acceptance.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    if (qEnvironmentVariableIsSet("CASSI_NATIVE_CACHE_CHILD_PHASE")) {
        NativeCacheWindowWorker worker;
        return QTest::qExec(&worker, argc, argv);
    }
    NativeWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "native-window-test.moc"
