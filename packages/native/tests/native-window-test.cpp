#include "mock-server.h"
#include "native-app.h"
#include "native-updater.h"
#include "native-controller.h"
#include "native-chat-crypto.h"
#include "native-markdown.h"
#include "native-cache-service.h"
#include "native-preferences.h"
#include "file-client.h"
#include "avatar-images.h"
#include "message-images.h"

#include <QCryptographicHash>
#include <QColor>
#include <QImage>
#include <QInputMethodEvent>
#include <QtMath>
#include <QApplication>
#include <QAccessible>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QPlatformSurfaceEvent>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
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
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QPointer>
#include <QSGRendererInterface>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTextOption>
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

static QQuickItem *visibleItemWithPrefix(QQuickItem *root, const QString &prefix)
{
    if (root->isVisible() && root->objectName().startsWith(prefix)) return root;
    for (auto *child : root->childItems())
        if (auto *found = visibleItemWithPrefix(child, prefix)) return found;
    return nullptr;
}

static QQuickItem *visibleItemWithPrefix(const QString &prefix)
{
    for (auto *candidate : QGuiApplication::allWindows())
        if (auto *window = qobject_cast<QQuickWindow *>(candidate))
            if (auto *found = visibleItemWithPrefix(window->contentItem(), prefix)) return found;
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

// Draft HTTP remains synthetic and is layered over the same catalog, topics,
// session and actual Main.qml harness as ordinary conversation tests.
class WindowDraftFixture : public WindowChannelFixture {
public:
    QHash<QString, QJsonObject> drafts;
    int draftLists = 0, draftPuts = 0, draftDeletes = 0;
    bool ownedDraftRequests = true, holdNextPut = false;
    QVector<QByteArray> mutationRevisions;
    std::function<void()> pendingPut;

    static QJsonObject row(const QString &id, const QString &text, int revision = 1, int second = 1)
    {
        return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"stream_uuid", streamId},
            {"topic_uuid", topicId}, {"revision", revision}, {"created_at", "2026-01-02T10:00:00.000Z"},
            {"updated_at", QString("2026-01-02T10:00:%1.123456Z").arg(second, 2, 10, QLatin1Char('0'))},
            {"payload", QJsonObject{{"kind", "markdown"}, {"content", text}}}};
    }
    static Response snapshot(const QJsonObject &value, int status = 200)
    {
        Response response{status == 412 ? QJsonObject{{"current", value}} : value, status};
        response.headers.insert("ETag", '"' + QByteArray::number(value.value("revision").toInt()) + '"');
        return response;
    }
    WindowDraftFixture()
    {
        sourceScene = true;
        const auto fallback = server.handler;
        server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (!path.startsWith("/api/workspace/v1/messenger/drafts/")) return fallback(request);
            ownedDraftRequests &= !QUrlQuery(request.target).hasQueryItem("project_id");
            if (!ownedDraftRequests) return {QJsonObject{}, 403};
            const auto id = path.section('/', -1);
            if (request.method == "GET" && id.isEmpty()) {
                ++draftLists; QJsonArray rows;
                auto ids = drafts.keys(); ids.sort();
                for (const auto &key : ids) rows.append(drafts.value(key));
                return {rows};
            }
            if (!drafts.contains(id)) return {QJsonObject{}, 404};
            if (request.method == "GET") return snapshot(drafts[id]);
            mutationRevisions.append(request.headers.value("if-match"));
            if (request.method == "PUT") ++draftPuts;
            if (request.method == "DELETE") ++draftDeletes;
            const auto revision = '"' + QByteArray::number(drafts[id].value("revision").toInt()) + '"';
            if (request.headers.value("if-match") != revision) return snapshot(drafts[id], 412);
            if (request.method == "PUT") {
                drafts[id]["payload"] = request.body.value("payload");
                drafts[id]["revision"] = drafts[id].value("revision").toInt() + 1;
                auto response = snapshot(drafts[id]);
                if (holdNextPut) {
                    holdNextPut = false;
                    response.defer = [this](auto send) { pendingPut = std::move(send); };
                }
                return response;
            }
            if (request.method == "DELETE") { drafts.remove(id); return {QJsonObject{}, 204}; }
            return {QJsonObject{}, 405};
        };
    }
    void finishPut()
    {
        auto send = std::move(pendingPut); pendingPut = {};
        if (send) send();
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
    bool applyRestore=true,holdRestores=false;
    QMap<QString,int> rowStatuses,rowDelays;
    QMap<QString,std::function<void()>> pendingRestores;
    void finishRestore(const QString &id) { auto send=pendingRestores.take(id);if(send)send(); }
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
                    Response response{row,rowStatuses.value(id,restoreStatus),rowDelays.value(id,restoreDelay)};
                    if(holdRestores)response.defer=[this,id](auto send){pendingRestores[id]=std::move(send);};
                    return response;
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
    auto *dialog = window->findChild<QObject *>("createChatDialog"); QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QSignalSpy painted(window, &QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!painted.isEmpty());
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

static const QString ViewerAvatarFile = "a4300000-0000-4000-8000-000000000001";
static const QString PeerAvatarFile = "a4300000-0000-4000-8000-000000000002";
static QByteArray avatarProbePng(bool peer)
{
    // Exact 96x64 RGBA fixtures; guard bands distinguish center-cover from stretch.
    return QByteArray::fromHex(peer ?
        "89504e470d0a1a0a0000000d4948445200000060000000400806000000e534720e000000ad4944415478daedd1310d80301440c19a68d28d818da4766a017d4c18a8061430b023004474f81d6e7806dea5be5ddf48ebfe84f69e25b4d17f090000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000009800e05e6a68f968a1010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001300fc9d730b851847383f0000000049454e44ae426082" :
        "89504e470d0a1a0a0000000d4948445200000060000000400806000000e534720e000000ad4944415478daedd1310d80301440c19a68d28d818da4766a017d4c18a8061430b023004474f81d6e7806dea5be5ddf48f75243cb470b6df45f02000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000260058f727b4f72ca1010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001300fc64110b8532ff49270000000049454e44ae426082");
}

static QJsonObject avatarProbeMetadata(const QString &uuid, const QByteArray &png)
{
    return {{"uuid", uuid}, {"stream_uuid", QJsonValue::Null}, {"name", "public-avatar.png"},
        {"content_type", "image/png"}, {"size_bytes", png.size()},
        {"hash", QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex())}};
}

// A markdown row carrying one standalone image (media viewer tests, issue #8).
static QJsonObject mediaViewerRow(const QString &uuid, const QString &fileUuid, qint64 byteSize)
{
    const auto urn = QStringLiteral("urn:image:%1?name=shot.png&content_type=image%2Fpng&w=96&h=64&size=%2")
        .arg(fileUuid, QString::number(byteSize));
    auto row = message(QStringLiteral("Look\n\n![shot.png](%1)").arg(urn), uuid);
    row["user_uuid"] = userId;
    row["author_uuid"] = channelPeer; row["is_own"] = false;
    row["pinned"] = false; row["reactions"] = QJsonObject{};
    row["reaction_users"] = QJsonObject{}; row["updated_at"] = row.value("created_at");
    return row;
}

static QQuickItem *avatarProbeCircle(QQuickItem *root, qreal diameter)
{
    if (!root || !root->isVisible()) return nullptr;
    if (root->size() == QSizeF(diameter, diameter)
        && root->property("radius").toReal() == diameter / 2) return root;
    for (auto *child : root->childItems())
        if (auto *found = avatarProbeCircle(child, diameter)) return found;
    return nullptr;
}

static QQuickItem *avatarProbeReadyImage(QQuickItem *root)
{
    if (!root || !root->isVisible()) return nullptr;
    const auto source = root->property("source").toUrl();
    // Image.status is the public QML enum: Null=0, Ready=1, Loading=2, Error=3.
    if (source.scheme() == "image" && source.host() == "avatars"
        && root->property("status").toInt() == 1) return root;
    for (auto *child : root->childItems())
        if (auto *found = avatarProbeReadyImage(child)) return found;
    return nullptr;
}

static bool avatarProbeHasVisibleLock(QQuickItem *root)
{
    if (!root || !root->isVisible()) return false;
    if (root->property("asset").toString() == "chat_lock.svg") return true;
    for (auto *child : root->childItems()) if (avatarProbeHasVisibleLock(child)) return true;
    return false;
}

static bool avatarProbeMatchesPixels(QQuickWindow *window, QQuickItem *face, bool peer)
{
    if (!window || !face || !face->isVisible()) return false;
    const auto image = window->grabWindow();
    if (image.isNull()) return false;
    const auto dpr = window->devicePixelRatio();
    const auto sample = [&](const QPointF &local) -> QColor {
        const auto scene = face->mapToScene(local) * dpr;
        const QPoint pixel(qFloor(scene.x()), qFloor(scene.y()));
        return image.rect().contains(pixel) ? image.pixelColor(pixel) : QColor{};
    };
    const auto close = [](const QColor &a, const QColor &b) {
        return a.isValid() && b.isValid() && qAbs(a.red() - b.red()) <= 3
            && qAbs(a.green() - b.green()) <= 3 && qAbs(a.blue() - b.blue()) <= 3;
    };
    const QColor red(228,31,45), green(21,177,86), blue(35,103,232), gold(243,183,24);
    const QVector<QColor> colors = peer ? QVector<QColor>{blue,gold,red,green}
                                       : QVector<QColor>{red,green,blue,gold};
    const QSizeF inner(face->width()-2, face->height()-2);
    const QVector<QPointF> locations{{.30,.28},{.70,.28},{.30,.72},{.70,.72},{.12,.28},{.88,.28}};
    const int expected[]{0,1,2,3,0,1};
    for (qsizetype i=0; i<locations.size(); ++i) {
        const QPointF local(1 + locations[i].x()*inner.width(), 1 + locations[i].y()*inner.height());
        if (!close(sample(local), colors[expected[i]])) return false;
    }
    // These corners are outside the circle. Compare with adjacent real parent
    // background, not a guessed theme/hover surface. Square image leakage fails.
    return close(sample(QPointF(2,2)), sample(QPointF(-2,2)));
}

static bool presenceProbeMatchesPixels(QQuickWindow *window, QQuickItem *badge,
                                      const QColor &fill, const QColor &ring)
{
    if (!window || !badge || !badge->isVisible()) return false;
    const auto pixels=window->grabWindow();
    if(pixels.isNull()) return false;
    const auto sample=[&](QPointF local) {
        const auto point=badge->mapToScene(local)*window->devicePixelRatio();
        const QPoint pixel(qFloor(point.x()),qFloor(point.y()));
        return pixels.rect().contains(pixel)?pixels.pixelColor(pixel):QColor{};
    };
    const auto close=[](const QColor &actual,const QColor &expected) {
        return actual.isValid()&&qAbs(actual.red()-expected.red())<=3
            &&qAbs(actual.green()-expected.green())<=3&&qAbs(actual.blue()-expected.blue())<=3;
    };
    // Stable interiors only; no cross-renderer antialiasing or exact edge claim.
    return close(sample({6,6}),fill)&&close(sample({11,6}),ring);
}


class WindowOwnStatusFixture {
public:
    QJsonObject own{{"uuid",userId},{"project_id",projectId},{"username","cassi"},
        {"first_name","Cassi"},{"last_name","Synthetic"},{"email","cassi@example.invalid"},
        {"avatar",QJsonValue::Null},{"source","iam"},{"status","active"},
        {"status_text","Before"},{"status_emoji","💬"},
        {"last_ping_at","2026-01-02T10:00:00Z"},{"created_at","2026-01-02T10:00:00Z"},
        {"updated_at","2026-01-02T10:00:00Z"}};
    QJsonObject posted;
    int posts=0,usersGets=0,status=200;
    bool held=false;
    std::function<void()> release;
    MockServer server; // Captured facts outlive server/request callbacks.
    WindowOwnStatusFixture() {
        server.handler=[this](const Request &request)->Response {
            const auto path=request.target.path();
            if(path=="/api/workspace/v1/users/"+userId+"/actions/presence/invoke") {
                ++posts;posted=request.body;
                auto row=own;row["status"]=posted.value("status");row["status_text"]=posted.value("text");
                row["status_emoji"]=posted.value("emoji");row["updated_at"]="2026-01-02T10:01:00Z";
                row["last_ping_at"]="2026-01-02T10:01:00Z";
                Response response{row,status};
                if(held)response.defer=[this](std::function<void()> send){release=std::move(send);};
                return response;
            }
            if(path=="/api/workspace/v1/users/"){++usersGets;return {QJsonArray{own}};}
            if(path=="/api/workspace/v1/messenger/streams/")return {QJsonArray{windowChannel(streamId,"Status workspace")}};
            if(path=="/api/workspace/v1/messenger/streams/"+streamId)return {windowChannel(streamId,"Status workspace")};
            if(path=="/api/workspace/v1/messenger/stream_topics/")return {QJsonArray{windowChannelTopic(topicId,streamId,"General")}};
            if(path=="/api/workspace/v1/messenger/stream_topics/"+topicId)return {windowChannelTopic(topicId,streamId,"General")};
            if(path.endsWith("/messages/"))return {QJsonArray{}};
            return sessionResponse(request);
        };
    }
    bool start(NativeController &controller) {
        controller.login(server.origin().toString(),"synthetic-user","synthetic-password","");
        if(!QTest::qWaitFor([&]{return controller.projects().size()==1;},2000))return false;
        controller.openProject(projectId);
        if(!QTest::qWaitFor([&]{return controller.streams().size()==1;},2000))return false;
        controller.selectStream(streamId);
        if(!QTest::qWaitFor([&]{return controller.topics().size()==1;},2000))return false;
        controller.selectTopic(topicId);
        return QTest::qWaitFor([&]{return !controller.busy()&&usersGets==1;},2000);
    }
    QPointer<QNetworkReply> pending(ApiClient &api) const {
        for(auto *reply:api.findChildren<QNetworkReply *>())
            if(!reply->isFinished()&&reply->url().path()=="/api/workspace/v1/users/"+userId+"/actions/presence/invoke"
                &&reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray()=="POST")return reply;
        return {};
    }
    ~WindowOwnStatusFixture(){if(release)release();}
};

// Additive test helpers only; the old disabled control is found on unchanged Main.
static QQuickItem *composerEmojiTrigger(QQuickItem *root)
{
    if (!root) return nullptr;
    if (root->isVisible() && root->property("asset").toString()=="composer-emoji.svg") return root;
    for (auto *child:root->childItems()) if(auto *found=composerEmojiTrigger(child)) return found;
    return nullptr;
}
static QQuickItem *composerEmojiContent(QQuickWindow *window)
{
    auto *popup=window->findChild<QObject *>("composerEmojiPicker");
    return popup&&popup->property("visible").toBool()?popup->property("contentItem").value<QQuickItem *>():nullptr;
}

static bool composerEmojiChoiceReady(QQuickWindow *window,QQuickItem *picker)
{
    auto *choice=picker?item(picker,"emoji-choice-grinning"):nullptr;
    auto *grid=picker?item(picker,"reactionGrid"):nullptr;
    return choice&&grid&&choice->isVisible()&&choice->isEnabled()&&choice->width()>0&&choice->height()>0
        &&QRectF(QPointF{},grid->size()).contains(QRectF(choice->mapToItem(grid,QPointF{}),choice->size()))
        &&QRectF(QPointF{},QSizeF(window->size())).contains(choice->mapToScene(QPointF(choice->width()/2,choice->height()/2)));
}
static void captureComposerEmojiFrame(QQuickWindow *window,const QString &locale,const QString &mode,const QString &state)
{
    const auto directory=qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");if(directory.isEmpty())return;
    QSignalSpy frame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!frame.isEmpty());
    const auto base=QDir(directory).filePath("native-composer-emoji-"+locale+"-"+mode+"-"+state);
    QVERIFY(window->grabWindow().save(base+".png"));
    QJsonObject data{{"synthetic",true},{"fullShellParity",false},{"liveAccountAcceptance",false},
        {"locale",locale},{"mode",mode},{"state",state},{"dpr",window->devicePixelRatio()},
        {"width",window->width()},{"height",window->height()},
        {"graphicsApi",int(window->rendererInterface()->graphicsApi())}};
    if(auto *popup=window->findChild<QObject *>("composerEmojiPicker"))
        data["composerEmojiPicker"]=QJsonObject{{"x",popup->property("x").toDouble()},{"y",popup->property("y").toDouble()},
            {"width",popup->property("width").toDouble()},{"height",popup->property("height").toDouble()}};
    for(const auto &name:QStringList{"reactionSearch","reactionGrid","messageComposer"})
        if(auto *found=visibleItem(name)){const auto at=found->mapToScene(QPointF{});data[name]=QJsonObject{{"x",at.x()},{"y",at.y()},{"width",found->width()},{"height",found->height()}};}
    QFile output(base+".json");QVERIFY(output.open(QIODevice::WriteOnly));output.write(QJsonDocument(data).toJson());
}

static bool startComposerEmojiFixture(WindowChannelFixture &fixture,NativeController &controller,Session &session)
{
    session.login(fixture.server.origin(),"synthetic-composer","synthetic-password");
    if(!QTest::qWaitFor([&]{return session.projects().size()==1;},2000))return false;
    session.selectProject(projectId);
    if(!QTest::qWaitFor([&]{return session.ready()&&!controller.busy()&&!controller.streams().isEmpty();},2000))return false;
    controller.setRealtimeMode(true);
    controller.selectStream(streamId);
    if(!QTest::qWaitFor([&]{return !controller.busy()&&controller.topics().size()==3;},2000))return false;
    controller.selectTopic(topicId);
    return QTest::qWaitFor([&]{return !controller.busy()&&controller.hasConversation();},2000);
}

class NativeDraftWorkload final : public QObject {
    Q_OBJECT
private slots:
    void boundedOrdinaryComposerNavigation()
    {
        QTest::failOnWarning();
        WindowChannelFixture fixture;
        const auto topicFor = [](int i) { return QString("a3000000-0000-4000-8000-%1").arg(i, 12, 16, QLatin1Char('0')); };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&, fallback](const Request &request) -> Response {
            if (request.target.path().endsWith("/stream_topics/")) {
                QJsonArray rows{windowChannelTopic(topicId, streamId, "General")};
                for (int i = 0; i < 160; ++i) {
                    auto row = windowChannelTopic(topicFor(i), streamId, "Draft " + QString::number(i));
                    row["is_default"] = false; rows.append(row);
                }
                return {rows};
            }
            if (request.method == "GET" && request.target.path().endsWith("/messages/")) return {QJsonArray{}};
            return fallback(request);
        };
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        NativeCacheService cache(directory.filePath("cache"), NativeCacheService::Policy{}, true);
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        controller.useCache(&cache); controller.useDrafts(&drafts);
        NativePreferences preferences(directory.filePath("appearance.ini")); preferences.setLocale("en");
        bindNativeCachePreferences(cache, preferences);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data())) QTest::qFail("Draft workload teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "cassi-native-workload", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(session.ready() && !controller.busy() && !controller.streams().isEmpty());
        QTRY_VERIFY(cache.available()); QCOMPARE(cache.appliedDiskLimitBytes(), 10000000000LL);
        controller.setRealtimeMode(true); controller.selectStream(streamId); QTRY_COMPARE(controller.topics().size(), 161);
        QElapsedTimer elapsed; elapsed.start(); QJsonArray samples;
        const auto sample = [&](int conversations) {
            const auto model = drafts.residency(); const auto memory = controller.draftMemory();
            const auto history = cache.stats();
            qint64 rss = -1;
#ifdef Q_OS_LINUX
            QFile status("/proc/self/status");
            if (status.open(QIODevice::ReadOnly)) {
                const auto match = QRegularExpression("VmRSS:\\s+(\\d+)").match(QString::fromUtf8(status.readAll()));
                if (match.hasMatch()) rss = match.captured(1).toLongLong();
            }
#endif
            samples.append(QJsonObject{{"conversations", conversations}, {"elapsed_ms", elapsed.elapsed()}, {"rss_kib", rss},
                {"resident_records", model.records}, {"cached_records", model.cachedRecords}, {"cached_payload_bytes", model.cachedBytes},
                {"cached_routes", model.cachedRoutes}, {"controller_text_entries", memory.entries}, {"controller_text_bytes", memory.textBytes},
                {"history_cache_entries", history.entries}, {"history_cache_hits", qint64(history.hits)},
                {"history_cache_stale_hits", qint64(history.staleHits)}});
        };
        sample(0);
        for (int i = 0; i < 160; ++i) {
            controller.selectTopic(topicFor(i)); QTRY_VERIFY(drafts.ready(streamId, topicFor(i)) && controller.canCompose());
            QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
            auto *composer = item(window->contentItem(), "messageComposer"); QVERIFY(composer && composer->isEnabled());
            composer->forceActiveFocus(Qt::OtherFocusReason);
            const auto body = QString::number(i) + QString(39950, QChar(0x754c));
            const auto requests = fixture.server.requests.size();
            QInputMethodEvent input; input.setCommitString(body); QCoreApplication::sendEvent(composer, &input);
            QCOMPARE(controller.draft(), body); QCOMPARE(fixture.server.requests.size(), requests);
            if ((i + 1) % 32 == 0) {
                bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
                bool historyDone = false; cache.flush(this, [&](bool okay) { historyDone = okay; }); QTRY_VERIFY(historyDone);
                QTest::qWait(50); sample(i + 1);
                QVERIFY(cache.stats().entries > 0);
                QVERIFY(drafts.residency().cachedRecords <= NativeDrafts::MaximumCachedRecords);
                QVERIFY(drafts.residency().cachedBytes <= NativeDrafts::MaximumCachedBytes);
                QVERIFY(drafts.residency().cachedRoutes <= NativeDrafts::MaximumCachedRoutes);
                QVERIFY(controller.draftMemory().entries <= 1);
                QVERIFY(controller.draftMemory().verifiedRoutes <= NativeDrafts::MaximumCachedRoutes + 1);
            }
        }
        controller.selectTopic(topicFor(0)); QTRY_VERIFY(drafts.ready(streamId, topicFor(0)) && controller.canCompose());
        QCOMPARE(controller.draft(), QString("0") + QString(39950, QChar(0x754c)));
        QTRY_VERIFY(cache.stats().hits + cache.stats().staleHits > 0);
        QVERIFY(controller.error().isEmpty());
        const QJsonObject report{{"scope", "Actual Main.qml/controller/cache/draft workers; synthetic local HTTP; not live account acceptance"},
            {"samples", samples}, {"network_requests", fixture.server.requests.size()},
            {"cache_limit_bytes", cache.appliedDiskLimitBytes()}};
        const auto output = qEnvironmentVariable("CASSI_NATIVE_DRAFT_WORKLOAD_REPORT");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(QFileInfo(output).absolutePath())); QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(report).toJson());
        }
        bool stopped = false; shutdownNativeStorage(drafts, storage, cache, this, [&](bool okay) { stopped = okay; }); QTRY_VERIFY(stopped);
    }
};

// Issue #146: drives the transport/rate-limit/bootstrap shapes the top
// overlay banners react to, over the same Main.qml harness as other window
// tests (drop = one aborted streams GET, break = malformed catalog row).
class WindowBannerFixture {
public:
    MockServer server;
    int streamsGets = 0, rateLimitAfterSeconds = 0;
    bool breakStreams = false, dropRound = false;
    WindowBannerFixture() {
        server.handler = [this](const Request &request) -> Response {
            const auto path = request.target.path();
            // Outage window: every GET dies at the transport level, so no
            // unrelated in-flight response can clear the offline phase.
            if (dropRound && request.method == "GET") {
                Response dropped; dropped.abortConnection = true; return dropped;
            }
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return {windowChannel(streamId, "Banner workspace")};
            if (path.endsWith("/streams/") && request.method == "GET") {
                ++streamsGets;
                if (breakStreams)
                    return {QJsonArray{QJsonObject{{"uuid", QStringLiteral("not-a-uuid")}}}};
                if (rateLimitAfterSeconds > 0) {
                    Response limited{QJsonArray{}, 429};
                    limited.headers.insert("Retry-After", QByteArray::number(rateLimitAfterSeconds));
                    return limited;
                }
                return {QJsonArray{windowChannel(streamId, "Banner workspace")}};
            }
            if (path.endsWith("/users/"))
                return {QJsonArray{QJsonObject{{"uuid", userId}, {"username", "cassi"}}}};
            if (path.endsWith("/stream_topics/"))
                return {QJsonArray{windowChannelTopic(topicId, streamId, "General")}};
            if (path.endsWith("/messages/")) return {QJsonArray{}};
            return sessionResponse(request);
        };
    }
    bool start(NativeController &controller) {
        controller.login(server.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([&] { return controller.projects().size() == 1; }, 2000)) return false;
        controller.openProject(projectId);
        if (!QTest::qWaitFor([&] { return controller.streams().size() == 1; }, 2000)) return false;
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([&] { return !controller.topics().isEmpty(); }, 2000)) return false;
        controller.selectTopic(topicId);
        return QTest::qWaitFor([&] { return !controller.busy(); }, 2000);
    }
};

// Issue #132: channel info drawer content on the ordinary channel scene.
// Serves a stream with four members (one online twin pair), a user directory
// with presence statuses and emails, one addable stranger (dave), plus the
// notification invoke, add_users and stream binding DELETE endpoints.
// Geometry oracles live in the test cases and cite
// packages/web/src/widgets/right-panel/right-panel-workspace-info.ui.tsx and
// packages/web/src/shared/ui/notification-level-switch/
// notification-level-switch.lib.ts at 61dacef78dab2f5d0be76f283bebf49fe29b84af.
class WindowChannelInfoFixture : public WindowChannelFixture {
public:
    static QString bindingId(int number)
    {
        return QStringLiteral("83000000-0000-4000-8000-%1")
            .arg(number, 12, 10, QLatin1Char('0'));
    }
    static QString extraUser(int number)
    {
        return QStringLiteral("9a000000-0000-4000-8000-%1")
            .arg(number, 12, 10, QLatin1Char('0'));
    }
    QString bobUser = extraUser(2);
    QString carolUser = extraUser(3);
    QString daveUser = extraUser(4);
    // Second owner stream: switching here while a request for the first stream
    // is in flight reproduces the review #186 busy-leak races (F1/F2).
    QString secondStreamId = QStringLiteral("3e0d0000-0000-4000-8000-000000000002");
    QString secondTopic = secondTopicId;
    QJsonObject streamRow;
    QJsonObject secondStreamRow;
    QJsonArray members;
    QJsonArray servedMembers;
    QJsonArray secondMembers;
    int notifPosts = 0, removeDeletes = 0, memberDelay = 120;
    int notifStatus = 200;
    QJsonObject notifBody;
    WindowChannelInfoFixture(bool streamOwner = true)
    {
        streamRow = windowChannel(streamId, "Info channel");
        // color oracle: resolveAvatarBackgroundColor(0x4b98e6) -> #4b98e6
        // (packages/web/src/shared/lib/avatar-color.lib.ts).
        streamRow["description"] = "Synthetic purpose.";
        streamRow["color"] = 0x4b98e6;
        streamRow["role"] = streamOwner ? "owner" : "member";
        secondStreamRow = windowChannel(secondStreamId, "Second channel");
        secondStreamRow["role"] = "owner";
        secondMembers = QJsonArray{bindingRow(bindingId(11), userId, "owner", secondStreamId),
            bindingRow(bindingId(12), channelPeer, "member", secondStreamId)};
        users = QJsonArray{
            userRow(userId, "cassi", "Cassi", "active", "cassi@example.invalid"),
            userRow(channelPeer, "alice", "Alice", "active", "alice@example.invalid"),
            userRow(bobUser, "bob", "Bob", "idle", "bob@example.invalid"),
            userRow(carolUser, "carol", "Carol", QJsonValue(), "carol@example.invalid"),
            userRow(daveUser, "dave", "Dave", "offline", "dave@example.invalid")};
        members = QJsonArray{
            bindingRow(bindingId(1), userId, streamOwner ? "owner" : "member"),
            bindingRow(bindingId(2), channelPeer, "member"),
            bindingRow(bindingId(3), bobUser, "administrator"),
            bindingRow(bindingId(4), carolUser, "member")};
        servedMembers = members;
        auto base = server.handler;
        server.handler = [this, base](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/streams/") && !path.contains("/messages/"))
                return {QJsonArray{streamRow, secondStreamRow}};
            if (request.method == "GET" && path.endsWith("/streams/" + streamId))
                return {streamRow};
            if (request.method == "GET" && path.endsWith("/streams/" + secondStreamId))
                return {secondStreamRow};
            if (request.method == "GET" && path.endsWith("/stream_bindings/")) {
                if (QUrlQuery(request.target).queryItemValue("stream_uuid") == secondStreamId)
                    return {secondMembers, 200, memberDelay};
                return {servedMembers, 200, memberDelay};
            }
            if (request.method == "GET" && path.endsWith("/stream_topics/")
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == secondStreamId)
                return {QJsonArray{windowChannelTopic(secondTopic, secondStreamId, "General two")}};
            if (request.method == "POST" && path.endsWith("/actions/notifications/invoke")) {
                ++notifPosts;
                notifBody = request.body;
                // streams/{uuid}/actions/notifications/invoke: the invoke
                // answers with the canonical DTO of the stream it addressed.
                auto row = path.contains(secondStreamId) ? secondStreamRow : streamRow;
                row["notification_mode"] = request.body.value("notification_mode");
                return {row, notifStatus, notifStatus == 200 ? 180 : 160};
            }
            if (request.method == "DELETE" && path.contains("/stream_bindings/")) {
                ++removeDeletes;
                const auto uuid = path.section(QLatin1Char('/'), -1);
                QJsonArray rest;
                for (const auto &value : members)
                    if (value.toObject().value("uuid").toString() != uuid) rest.append(value);
                members = rest;
                servedMembers = rest;
                return {QJsonObject{}, 200};
            }
            if (request.method == "POST" && path.endsWith("/actions/add_users/invoke")) {
                ++memberPosts;
                memberBody = request.body;
                // streams/{uuid}/actions/add_users/invoke binds on the stream
                // it addressed (path field -4 = {uuid}).
                const auto target = path.section(QLatin1Char('/'), -4, -4);
                QJsonArray rows;
                int number = 5;
                for (const auto &value : request.body.value("member").toArray()) {
                    const auto row = bindingRow(QStringLiteral("83000000-0000-4000-8000-%1")
                        .arg(number++, 12, 10, QLatin1Char('0')), value.toString(), "member", target);
                    rows.append(row);
                    if (target == secondStreamId) secondMembers.append(row);
                    else { members.append(row); servedMembers.append(row); }
                }
                return {rows, 200, 120};
            }
            return base(request);
        };
    }
    static QJsonObject userRow(const QString &id, const QString &username, const QString &name,
                               const QJsonValue &status, const QString &email)
    {
        QJsonObject row{{"uuid", id}, {"username", username}, {"first_name", name},
            {"source", "iam"}, {"avatar", QJsonValue::Null}, {"email", email},
            {"last_ping_at", "2026-01-02T10:00:00Z"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        if (!status.isNull()) row["status"] = status;
        return row;
    }
    // Binding DTO fields as validated by creationBindings (native-controller.cpp)
    // and the reference WorkspaceMessengerStreamBindingDto.
    static QJsonObject bindingRow(const QString &id, const QString &user, const QString &role,
                                  const QString &stream = streamId)
    {
        return {{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream},
            {"user_uuid", user}, {"who_uuid", userId}, {"role", role},
            {"notification_mode", "all_messages"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
    }
};

// Shared live window scene: sign in, select the channel and open the drawer's
// chat-info root screen where the channel info layer mounts.
struct ChannelInfoWindowScene {
    WindowChannelInfoFixture fixture;
    QTemporaryDir directory;
    NativePreferences preferences;
    ApiClient api{nullptr, true};
    Session session;
    NativeController controller;
    QQmlApplicationEngine engine;
    QPointer<QQuickWindow> window;
    explicit ChannelInfoWindowScene(bool streamOwner = true)
        : fixture(streamOwner), preferences(directory.filePath("channel-info.ini")),
          session(&api), controller(&api, &session)
    {
        QVERIFY(directory.isValid());
        preferences.setLocale("en");
        loadNativeUi(engine, &controller, &preferences);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        controller.login(fixture.server.origin().toString(), "synthetic-user",
            "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        // The fixture serves the info channel plus a switchable second owner
        // stream (review #186 busy-leak races); the scene starts on the first.
        QTRY_COMPARE(controller.streams().size(), 2);
        controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1);
        controller.selectTopic(topicId);
        QTRY_VERIFY(!controller.busy());
    }
    ~ChannelInfoWindowScene()
    {
        if (window && !closeIncubatedTestWindow(engine, window))
            QTest::qFail("channel info scene teardown", __FILE__, __LINE__);
    }
    QVariant evaluate(const QString &script)
    {
        QQmlExpression expression(qmlContext(window), window, script);
        auto value = expression.evaluate();
        if (expression.hasError())
            QTest::qFail(qPrintable("QML: " + expression.error().toString()), __FILE__, __LINE__);
        if (value.metaType() == QMetaType::fromType<QJSValue>())
            value = value.value<QJSValue>().toVariant();
        return value;
    }
    void openChannelInfo()
    {
        // chat-info seeds a single info screen (NativeRightDrawer.screensFor),
        // so openInfo lands directly on the channel info root screen.
        evaluate("rightDrawer.openInfo()");
        QTRY_VERIFY(visibleItem("channelInfoLayer"));
    }
    QString text(const QString &objectName)
    {
        auto *entry = visibleItem(objectName);
        return entry ? entry->property("text").toString() : QString();
    }
};

class NativeWindowTest final : public QObject {
    Q_OBJECT
private slots:
    void aboutDrawerPreservesTheActualPendingCheck_data()
    {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::newRow("en-light") << QString("en") << QString("light");
        QTest::newRow("ru-dark") << QString("ru") << QString("dark");
    }
    void aboutDrawerPreservesTheActualPendingCheck()
    {
        QFETCH(QString,locale); QFETCH(QString,mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MockServer releases; std::function<void()> sendMetadata;
        auto releaseHeld=qScopeGuard([&]{if(sendMetadata)sendMetadata();});
        releases.handler=[&](const Request &)->Response {
            Response result{QJsonObject{{"tag_name","0.1.0"},{"draft",false},{"prerelease",false}}};
            result.defer=[&](std::function<void()> send){sendMetadata=std::move(send);}; return result;
        };
        NativeUpdater updater(releases.origin().resolved(QUrl("/latest")),"0.1.0",directory.filePath("updates"));
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session);
        NativePreferences preferences(directory.filePath("preferences.ini")); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences,nullptr,&updater);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("About pending check teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(window->setProperty("updatesVisible",true)); QTRY_VERIFY(visibleItem("nativeUpdateAction"));
        auto *drawer=visibleItem("nativeAboutDrawer");
        QVERIFY2(drawer,"Actual Source-shaped nonmodal About drawer is required");
        QCOMPARE(drawer->width(),qreal(315)); QCOMPARE(drawer->mapToScene(QPointF{}).x(),window->width()-315);
        auto *version=visibleItem("nativeAboutCurrentVersionText");QVERIFY(version);
        QCOMPARE(version->property("text").toString(),locale=="ru"?QString::fromUtf8("Текущая версия: 0.1.0"):QString("Current version: 0.1.0"));
        QCOMPARE(releases.requests.size(),0);
        click(window,visibleItem("nativeUpdateAction"));QTRY_COMPARE(updater.state(),QString("checking"));
        QTRY_VERIFY(bool(sendMetadata));QCOMPARE(releases.requests.size(),1);
        click(window,visibleItem("nativeUpdateClose"));QTRY_VERIFY(!visibleItem("nativeAboutDrawer"));
        QCOMPARE(updater.state(),QString("checking"));QCOMPARE(releases.requests.size(),1);
        QVERIFY(window->setProperty("updatesVisible",true));QTRY_VERIFY(visibleItem("nativeAboutDrawer"));
        QCOMPARE(updater.state(),QString("checking"));QCOMPARE(releases.requests.size(),1);
        auto send=std::move(sendMetadata);send();QTRY_COMPARE(updater.state(),QString("current"));
        QTRY_VERIFY(visibleItem("nativeUpdateStatus"));
        QTRY_COMPARE(visibleItem("nativeUpdateStatus")->property("text").toString(),locale=="ru"?QString::fromUtf8("Установлена последняя версия"):QString("You're up to date"));
        window->resize(700,600);QTRY_COMPARE(window->width(),700);
        drawer=visibleItem("nativeAboutDrawer");QVERIFY(drawer);QCOMPARE(drawer->width(),qreal(315));
        const QRectF bounds(drawer->mapToScene(QPointF{}),drawer->size());
        for(const auto &name:{QString("nativeUpdateClose"),QString("nativeUpdateAction")}){
            auto *action=visibleItem(name);QVERIFY(action);
            QVERIFY(bounds.contains(QRectF(action->mapToScene(QPointF{}),action->size())));
        }
        click(window,visibleItem("nativeUpdateClose"));QTRY_VERIFY(!visibleItem("nativeAboutDrawer"));
        QCOMPARE(releases.requests.size(),1);QCOMPARE(controller.authenticated(),false);
    }
    void aboutUpdateAutomaticallyDownloadsAndRequiresExplicitInstall_data()
    {
        aboutDrawerPreservesTheActualPendingCheck_data();
    }
    void aboutUpdateAutomaticallyDownloadsAndRequiresExplicitInstall()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);
        QTemporaryDir directory;QVERIFY(directory.isValid());WindowOwnStatusFixture fixture;
        MockServer releases;std::function<void()> sendArchive;
        auto releaseHeld=qScopeGuard([&]{if(sendArchive)sendArchive();});
        const QByteArray archive("verified bytes that are deliberately not a tar archive");
        const QJsonObject asset{{"name","Exordos-Workspace-Native-0.1.1-linux-x86_64.tar.gz"},
            {"size",archive.size()},{"digest","sha256:"+QString::fromLatin1(QCryptographicHash::hash(archive,QCryptographicHash::Sha256).toHex())},
            {"browser_download_url",releases.origin().resolved(QUrl("/archive")).toString()}};
        releases.handler=[&](const Request &request)->Response {
            if(request.target.path()=="/latest")return {QJsonObject{{"tag_name","0.1.1"},{"draft",false},{"prerelease",false},{"assets",QJsonArray{asset}}}};
            Response result;result.rawBody=archive;result.defer=[&](std::function<void()>send){sendArchive=std::move(send);};return result;
        };
        NativeUpdater updater(releases.origin().resolved(QUrl("/latest")),"0.1.0",directory.filePath("updates"));
        QSignalSpy restart(&updater,&NativeUpdater::restartRequested);
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        NativePreferences preferences(directory.filePath("preferences.ini"));preferences.setLocale(locale);preferences.setMode(mode);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences,nullptr,&updater);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("About automatic download teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        auto *profile=visibleItem("accountMenuButton");QVERIFY(profile);const auto profilePosition=profile->mapToScene(QPointF{});
        QVERIFY(!visibleItem("nativeAboutDrawer"));QCOMPARE(releases.requests.size(),0);
        updater.check();QTRY_COMPARE(updater.state(),QString("downloading"));
        QTRY_VERIFY(bool(sendArchive));QCOMPARE(releases.requests.size(),2);
        QVERIFY(!visibleItem("nativeAboutDrawer"));auto *indicator=visibleItem("nativeUpdateIndicator");QVERIFY(indicator);
        QCOMPARE(indicator->width(),qreal(36));QCOMPARE(indicator->height(),qreal(36));
        QCOMPARE(profile->mapToScene(QPointF{}),profilePosition);
        click(window,indicator);QTRY_VERIFY(visibleItem("nativeAboutDrawer"));QCOMPARE(releases.requests.size(),2);
        click(window,visibleItem("nativeUpdateClose"));QTRY_VERIFY(!visibleItem("nativeAboutDrawer"));
        QCOMPARE(updater.state(),QString("downloading"));QCOMPARE(releases.requests.size(),2);
        click(window,visibleItem("nativeUpdateIndicator"));QTRY_VERIFY(visibleItem("nativeAboutDrawer"));
        auto send=std::move(sendArchive);send();QTRY_COMPARE(updater.state(),QString("downloaded"));
        QCOMPARE(restart.size(),0);QCOMPARE(releases.requests.size(),2);
        QTRY_VERIFY(visibleItem("nativeUpdateAction"));
        QTRY_COMPARE(visibleItem("nativeUpdateAction")->property("text").toString(),locale=="ru"?QString::fromUtf8("Установить и перезапустить"):QString("Install and restart"));
        click(window,visibleItem("nativeUpdateAction"));QTRY_COMPARE(updater.state(),QString("error"));
        QCOMPARE(restart.size(),0);QCOMPARE(releases.requests.size(),2);
        QVERIFY(QDir(directory.filePath("updates")).entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty());
    }

    void aboutInstallRestartsOnlyForTheExplicitMatchingAction_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<bool>("explicitAction");
        QTest::newRow("en-explicit")<<QString("en")<<true;
        QTest::newRow("ru-explicit")<<QString("ru")<<true;
        QTest::newRow("en-external-install")<<QString("en")<<false;
        QTest::newRow("ru-external-install")<<QString("ru")<<false;
    }
    void aboutInstallRestartsOnlyForTheExplicitMatchingAction()
    {
        QFETCH(QString,locale);QFETCH(bool,explicitAction);
        QTemporaryDir directory;QVERIFY(directory.isValid());
        const bool hadHome=qEnvironmentVariableIsSet("HOME"),hadData=qEnvironmentVariableIsSet("XDG_DATA_HOME");
        const auto oldHome=qgetenv("HOME"),oldData=qgetenv("XDG_DATA_HOME");
        auto restoreEnvironment=qScopeGuard([&]{
            if(hadHome)qputenv("HOME",oldHome);else qunsetenv("HOME");
            if(hadData)qputenv("XDG_DATA_HOME",oldData);else qunsetenv("XDG_DATA_HOME");
        });
        qputenv("HOME",directory.path().toUtf8());qputenv("XDG_DATA_HOME",directory.filePath("data").toUtf8());
        // A small real tar/installer contract exercises Main's intent boundary.
        // It is synthetic and does not replace installed GitHub release acceptance.
        const QString name="workspace-native-0.1.1-linux-x86_64",prefix=directory.filePath(name);
        QVERIFY(QDir().mkpath(prefix+"/bin"));
        const auto write=[](const QString &path,const QByteArray &bytes){
            QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
        };
        QVERIFY(write(prefix+"/VERSION","0.1.1\n"));
        QVERIFY(write(prefix+"/bin/workspace-native","#!/bin/sh\nexit 0\n"));
        QVERIFY(QFile::setPermissions(prefix+"/bin/workspace-native",QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        QVERIFY(write(prefix+"/install.sh",R"SH(#!/bin/bash
set -eu
test "$#" = 1 && test "$1" = --no-launch
src="$(cd -- "$(dirname -- "$0")" && pwd)"
root="$XDG_DATA_HOME/workspace-native-install"
mkdir -p "$root/versions/0.1.1"
cp -a "$src/VERSION" "$src/bin" "$root/versions/0.1.1/"
ln -s versions/0.1.1 "$root/.current-new"
mv -Tf "$root/.current-new" "$root/current"
)SH"));
        const auto archivePath=directory.filePath("archive.tar.gz");QProcess tar;
        auto environment=QProcessEnvironment::systemEnvironment();environment.remove("TAR_OPTIONS");environment.remove("GZIP");
        tar.setProcessEnvironment(environment);tar.start("/usr/bin/tar",{"-czf",archivePath,"-C",directory.path(),name});
        QVERIFY(tar.waitForFinished(2000));QCOMPARE(tar.exitStatus(),QProcess::NormalExit);QCOMPARE(tar.exitCode(),0);
        QFile archiveFile(archivePath);QVERIFY(archiveFile.open(QIODevice::ReadOnly));const auto archive=archiveFile.readAll();QVERIFY(!archive.isEmpty());
        MockServer releases;
        const QJsonObject asset{{"name","Exordos-Workspace-Native-0.1.1-linux-x86_64.tar.gz"},{"size",archive.size()},
            {"digest","sha256:"+QString::fromLatin1(QCryptographicHash::hash(archive,QCryptographicHash::Sha256).toHex())},
            {"browser_download_url",releases.origin().resolved(QUrl("/archive")).toString()}};
        releases.handler=[&](const Request &request)->Response{
            if(request.target.path()=="/latest")return {QJsonObject{{"tag_name","0.1.1"},{"draft",false},{"prerelease",false},{"assets",QJsonArray{asset}}}};
            Response result;result.rawBody=archive;return result;
        };
        NativeUpdater updater(releases.origin().resolved(QUrl("/latest")),"0.1.0",directory.filePath("updates"));
        QSignalSpy restart(&updater,&NativeUpdater::restartRequested);
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        NativePreferences preferences(directory.filePath("preferences.ini"));preferences.setLocale(locale);preferences.setMode(locale=="ru"?"dark":"light");
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences,nullptr,&updater);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("About restart intent teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        updater.check();QTRY_COMPARE(updater.state(),QString("downloaded"));QCOMPARE(restart.size(),0);
        QVERIFY(window->setProperty("updatesVisible",true));QTRY_VERIFY(visibleItem("nativeUpdateAction"));
        if(explicitAction)click(window,visibleItem("nativeUpdateAction"));else updater.install();
        QTRY_COMPARE(updater.state(),QString("ready"));QCOMPARE(restart.size(),explicitAction?1:0);
        if(explicitAction)QCOMPARE(restart.first().first().toString(),directory.filePath("data/workspace-native-install/current/bin/workspace-native"));
        // Repeated state publication and closing/reopening cannot reuse intent.
        QVERIFY(QMetaObject::invokeMethod(&updater,"changed",Qt::DirectConnection));
        click(window,visibleItem("nativeUpdateClose"));QTRY_VERIFY(!visibleItem("nativeAboutDrawer"));
        QVERIFY(window->setProperty("updatesVisible",true));QTRY_VERIFY(visibleItem("nativeAboutDrawer"));
        QCoreApplication::processEvents();QCOMPARE(restart.size(),explicitAction?1:0);QCOMPARE(releases.requests.size(),2);
        QVERIFY(updater.downloadedPath().isEmpty());
        QVERIFY(QDir(directory.filePath("updates")).entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty());
        for(const auto &request:releases.requests){
            QVERIFY(!request.headers.contains("authorization"));QVERIFY(!request.headers.contains("cookie"));
        }
    }

    void updaterActualMainShowsAndExecutesTheDownloadAndInstallActions_data()
    {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::newRow("en-light") << QString("en") << QString("light");
        QTest::newRow("ru-dark") << QString("ru") << QString("dark");
    }
    void updaterActualMainShowsAndExecutesTheDownloadAndInstallActions()
    {
        QFETCH(QString,locale); QFETCH(QString,mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MockServer releases;
        const QByteArray archive("verified bytes that are deliberately not a tar archive");
        const QJsonObject asset{{"name","Exordos-Workspace-Native-0.1.1-linux-x86_64.tar.gz"},
            {"size",archive.size()}, {"digest","sha256:" + QString::fromLatin1(QCryptographicHash::hash(archive,QCryptographicHash::Sha256).toHex())},
            {"browser_download_url",releases.origin().resolved(QUrl("/archive")).toString()}};
        releases.handler=[&](const Request &request) -> Response {
            if(request.target.path()=="/latest") return {QJsonObject{{"tag_name","v0.1.1"},{"draft",false},{"prerelease",false},{"assets",QJsonArray{asset}}}};
            Response result; result.rawBody=archive; return result;
        };
        NativeUpdater updater(releases.origin().resolved(QUrl("/latest")),"0.1.0",directory.filePath("updates"));
        ApiClient api(nullptr,true); Session session(&api); NativeController controller(&api,&session);
        NativePreferences preferences(directory.filePath("preferences.ini")); preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences,nullptr,&updater);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data())) QTest::qFail("Updater Main teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QCOMPARE(releases.requests.size(),0);
        updater.check(); QTRY_COMPARE(updater.state(),QString("downloaded"));
        QVERIFY(!visibleItem("nativeAboutDrawer"));
        QVERIFY(window->setProperty("updatesVisible",true));
        QTRY_VERIFY(visibleItem("nativeUpdateAction"));
        QCOMPARE(visibleItem("nativeUpdateAction")->property("text").toString(),locale=="ru"?QString::fromUtf8("Установить и перезапустить"):QString("Install and restart"));
        click(window,visibleItem("nativeUpdateAction")); QTRY_COMPARE(updater.state(),QString("error"));
        QVERIFY(visibleItem("nativeUpdateStatus")); QCOMPARE(releases.requests.size(),2);
        click(window,visibleItem("nativeUpdateClose")); QTRY_VERIFY(!visibleItem("nativeUpdateAction"));
        QCOMPARE(controller.authenticated(),false);
        QVERIFY(QDir(directory.filePath("updates")).entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty());
    }

    void ownStatusClearKeepsItsRenderedInkWhileSaveIsPending_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<QString>("mode");
        QTest::newRow("en-light")<<QString("en")<<QString("light");
        QTest::newRow("ru-dark")<<QString("ru")<<QString("dark");
    }
    void ownStatusClearKeepsItsRenderedInkWhileSaveIsPending()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);
        WindowOwnStatusFixture fixture;fixture.held=true;
        QTemporaryDir directory;QVERIFY(directory.isValid());NativePreferences preferences(directory.filePath("status-clear-paint.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Clear paint teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        preferences.setLocale(locale);preferences.setMode(mode);QCOMPARE(preferences.paletteId(),QString("blue-cold"));
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        const auto profile=controller.account();
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));
        click(window,visibleItem("ownStatusAction"));QTRY_VERIFY(visibleItem("ownStatusText"));
        QSignalSpy firstFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!firstFrame.isEmpty());
        QCOMPARE(window->size(),QSize(1440,900));QCOMPARE(window->devicePixelRatio(),qreal(1));
        auto *clear=visibleItem("ownStatusClear");QVERIFY(clear&&clear->isEnabled());
        QVERIFY(!clear->property("hovered").toBool());QVERIFY(!clear->property("visualFocus").toBool());
        const auto box=QRectF(clear->mapToScene(QPointF{}),clear->size());
        const auto crop=box.toAlignedRect();const auto caption=clear->property("text").toString();
        const auto beforeFont=clear->property("font").value<QFont>();
        const auto before=window->grabWindow();QVERIFY(!before.isNull());QVERIFY(before.rect().contains(crop));
        const auto bgPoint=(box.topLeft()+QPointF(4,box.height()/2)).toPoint();
        const auto background=before.pixelColor(bgPoint);
        const auto contrast=[background](const QColor &color){return qAbs(color.red()-background.red())
            +qAbs(color.green()-background.green())+qAbs(color.blue()-background.blue());};
        // The mask comes from actual baseline glyph ink, not a requested font
        // metric or a product opacity property. Caption/box/font remain identical.
        QVector<QPoint> ink; qint64 beforeInk=0;
        for(int y=crop.top();y<=crop.bottom();++y)for(int x=crop.left();x<=crop.right();++x) {
            const QPoint point(x,y);const auto weight=contrast(before.pixelColor(point));
            if(weight>=48){ink.append(point);beforeInk+=weight;}
        }
        QVERIFY(ink.size()>=20);QVERIFY(beforeInk>0);QCOMPARE(fixture.posts,0);
        click(window,visibleItem("ownStatusPreset-1"));
        click(window,visibleItem("ownStatusText"));QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const auto key:QByteArray("  Focusing  "))QTest::keyClick(window,key);
        click(window,visibleItem("ownStatusAway"));click(window,visibleItem("ownStatusSave"));
        QTRY_COMPARE(fixture.posts,1);QTRY_VERIFY(bool(fixture.release));QTRY_VERIFY(fixture.pending(api));
        QCOMPARE(fixture.posted,QJsonObject({{"status","idle"},{"emoji","🏠"},{"text","Focusing"}}));
        QVERIFY(controller.ownStatus().value("pending").toBool());QCOMPARE(controller.account(),profile);
        QSignalSpy heldFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!heldFrame.isEmpty());
        clear=visibleItem("ownStatusClear");QVERIFY(clear&&!clear->isEnabled());
        QVERIFY(!clear->property("hovered").toBool());QVERIFY(!clear->property("visualFocus").toBool());
        QCOMPARE(QRectF(clear->mapToScene(QPointF{}),clear->size()),box);
        QCOMPARE(clear->property("text").toString(),caption);QCOMPARE(clear->property("font").value<QFont>(),beforeFont);
        const auto held=window->grabWindow();QVERIFY(!held.isNull());QCOMPARE(held.pixelColor(bgPoint),background);
        qint64 heldInk=0;for(const auto &point:ink)heldInk+=contrast(held.pixelColor(point));
        const QJsonObject facts{{"locale",locale},{"mode",mode},{"inkPixels",int(ink.size())},
            {"beforeContrast",beforeInk},{"heldContrast",heldInk},{"heldRatio",double(heldInk)/double(beforeInk)},
            {"sameBox",true},{"sameCaptionAndFont",true},{"unhoveredUnfocusedBoth",true},
            {"actualHeldPost",bool(fixture.pending(api))},{"posts",fixture.posts}};
        qInfo().noquote()<<"OWN_STATUS_CLEAR_PAINT "<<QJsonDocument(facts).toJson(QJsonDocument::Compact);
        // Source Clear has no disabled opacity utility; identical ink should
        // remain. 5% tolerates integer RGB/AA noise, and rejects the old ~50% fade.
        QVERIFY2(qAbs(heldInk-beforeInk)*100<=beforeInk*5,"Same actual Clear glyph ink faded during the held POST");
        auto send=std::move(fixture.release);fixture.release={};send();
        QTRY_VERIFY(!visibleItem("ownStatusText"));QTRY_COMPARE(controller.ownStatus().value("text").toString(),QString("Focusing"));
        QCOMPARE(fixture.posts,1);QCOMPARE(fixture.usersGets,1);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }
    void ownStatusPickerMatchesMeasuredSourcePreviewAndSearch_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("firstOracle");
        for(const auto &first:{QString("preview"),QString("search")}) {
            const auto en=("en-light-"+first).toUtf8(),ru=("ru-dark-"+first).toUtf8();
            QTest::newRow(en.constData())<<QString("en")<<QString("light")<<first;
            QTest::newRow(ru.constData())<<QString("ru")<<QString("dark")<<first;
        }
    }
    void ownStatusPickerMatchesMeasuredSourcePreviewAndSearch()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);QFETCH(QString,firstOracle);
        WindowOwnStatusFixture fixture;
        QTemporaryDir directory;QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("picker-raster-search.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Picker raster/search teardown",__FILE__,__LINE__);
        });
        window->resize(1440,900);QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        preferences.setLocale(locale);preferences.setMode(mode);
        QCOMPARE(preferences.locale(),locale);QCOMPARE(preferences.mode(),mode);
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("💬"));
        const auto context=controller.ownStatusContextKey();const auto identity=session.publicProfile();
        QVERIFY(!context.isEmpty());const auto userGets=fixture.usersGets;
        const auto current=[&](const QString &name){return visibleItem(window->contentItem(),name);};
        click(window,current("accountMenuButton"));QTRY_VERIFY(current("ownStatusAction"));
        click(window,current("ownStatusAction"));QTRY_VERIFY(current("ownStatusChooseEmoji"));
        click(window,current("ownStatusChooseEmoji"));
        QTRY_VERIFY(current("reactionSearch")&&current("reactionSearch")->hasActiveFocus());
        QPointer<QQuickItem> picker=current("ownStatusEmojiPicker");QVERIFY(picker);
        QSignalSpy chosen(picker.data(),SIGNAL(chosen(QString)));QVERIFY(chosen.isValid());
        const auto rect=[](QQuickItem *entry){return QRectF(entry->mapToScene(QPointF{}),entry->size());};
        const auto jsonRect=[](const QRectF &box){return QJsonObject{{"x",box.x()},{"y",box.y()},
            {"width",box.width()},{"height",box.height()}};};
        QList<QRect> inkBounds,rois;QList<QRectF> emojiBoxes;QList<int> searchSizes;
        for(const auto &phase:{QString("default"),QString("hover")}) {
            if(phase=="hover") {
                auto *choice=current("emoji-choice-grinning");QVERIFY(choice);
                const auto target=choice->mapToScene({choice->width()/2,choice->height()/2});
                QVERIFY(QRectF(QPointF{},QSizeF(window->size())).contains(target));
                QTest::mouseMove(window,target.toPoint());
                QTRY_VERIFY(current("emoji-choice-grinning")&&current("emoji-choice-grinning")->property("hovered").toBool());
            }
            const auto expectedEmoji=phase=="default"?QString("😊"):QString("😀");
            const auto expectedLabel=phase=="default"?QString("What's your mood?"):QString("grinning face");
            QTRY_VERIFY(current("reactionPreviewEmoji")&&current("reactionPreviewEmoji")->property("text").toString()==expectedEmoji);
            QTRY_VERIFY(current("reactionPreviewLabel")&&current("reactionPreviewLabel")->property("text").toString()==expectedLabel);
            QSignalSpy frame(window,&QQuickWindow::frameSwapped);QVERIFY(frame.isValid());
            window->update();QTRY_VERIFY(!frame.isEmpty());
            const auto pixels=window->grabWindow();QVERIFY(!pixels.isNull());
            QCOMPARE(pixels.size(),QSize(1440,900));QCOMPARE(window->devicePixelRatio(),qreal(1));
            QCOMPARE(window->rendererInterface()->graphicsApi(),QSGRendererInterface::OpenGL);
            // Reacquire after the actual frame; no retained delegate pointers across input/layout.
            auto *emoji=current("reactionPreviewEmoji"),*label=current("reactionPreviewLabel");
            auto *preview=current("reactionPreview"),*search=current("reactionSearch");
            QVERIFY(picker&&emoji&&label&&preview&&search);
            const auto previewRect=rect(preview),labelRect=rect(label),emojiRect=rect(emoji);
            // Half-open right edge is the label's actual text origin (584 at the
            // matched geometry). Never scan gray caption LCD fringes as emoji ink.
            const int right=qFloor(labelRect.left()+label->property("leftPadding").toReal());
            const QRect roi(qCeil(previewRect.left()+1),qCeil(previewRect.top()+2),
                right-qCeil(previewRect.left()+1),qFloor(previewRect.bottom()-2)-qCeil(previewRect.top()+2));
            QVERIFY(roi.isValid()&&pixels.rect().contains(roi));QRect bounds;int count=0;
            for(int y=roi.top();y<=roi.bottom();++y)for(int x=roi.left();x<=roi.right();++x) {
                const auto color=pixels.pixelColor(x,y);
                if(color.alpha()>0&&color.red()>=160&&color.green()>=80&&color.blue()<=100
                    &&color.red()-color.blue()>=80){bounds=bounds.united(QRect(x,y,1,1));++count;}
            }
            const auto emojiFont=emoji->property("font").value<QFont>();const QFontInfo resolved(emojiFont);
            const auto searchFont=search->property("font").value<QFont>();
            inkBounds.append(bounds);rois.append(roi);emojiBoxes.append(emojiRect);searchSizes.append(searchFont.pixelSize());
            const QJsonObject measured{{"schema","native-picker-raster-search-v1"},{"locale",locale},{"mode",mode},
                {"firstOracle",firstOracle},{"phase",phase},{"frameCount",frame.size()},
                {"picker",jsonRect(rect(picker.data()))},{"preview",jsonRect(previewRect)},
                {"emojiBox",jsonRect(emojiRect)},{"label",jsonRect(labelRect)},{"roiHalfOpen",QJsonArray{roi.left(),roi.top(),right,roi.bottom()+1}},
                {"yellowMask","A>0,R>=160,G>=80,B<=100,R-B>=80"},{"yellowCount",count},
                {"yellowBoundsHalfOpen",QJsonArray{bounds.left(),bounds.top(),bounds.right()+1,bounds.bottom()+1}},
                {"yellowWidth",bounds.width()},{"yellowHeight",bounds.height()},
                {"previewFontPx",emojiFont.pixelSize()},{"previewResolvedFamily",resolved.family()},
                {"previewImplicitWidth",emoji->implicitWidth()},{"searchFontPx",searchFont.pixelSize()},
                {"searchResolvedFamily",QFontInfo(searchFont).family()},{"previewText",emoji->property("text").toString()},
                {"previewLabel",label->property("text").toString()},{"posts",fixture.posts},{"chosen",chosen.count()}};
            qInfo().noquote()<<"PICKER_RASTER_SEARCH"<<QJsonDocument(measured).toJson(QJsonDocument::Compact);
            const auto output=qEnvironmentVariable("CASSI_NATIVE_STATUS_SCREENSHOT_DIRECTORY");
            if(!output.isEmpty()) {
                QVERIFY(QDir().mkpath(output));
                const auto base=QDir(output).filePath("native-picker-raster-search-"+locale+"-"+mode+"-"+firstOracle+"-"+phase);
                QVERIFY(!QFile::exists(base+".png")&&!QFile::exists(base+".json"));
                QVERIFY(pixels.save(base+".png"));QFile file(base+".json");QVERIFY(file.open(QIODevice::WriteOnly));
                const auto bytes=QJsonDocument(measured).toJson();QCOMPARE(file.write(bytes),qint64(bytes.size()));
            }
            QCOMPARE(emojiRect.size(),QSizeF(45,45));QCOMPARE(previewRect.height(),qreal(70));
            QCOMPARE(picker->size(),QSizeF(404,320));
            QCOMPARE(emoji->mapToItem(picker.data(),QPointF{}),QPointF(11,262));
            QCOMPARE(resolved.family(),QString("Noto Color Emoji"));
            QVERIFY(search->property("text").toString().isEmpty());
            QCOMPARE(fixture.posts,0);QCOMPARE(chosen.count(),0);QCOMPARE(fixture.usersGets,userGets);
            QCOMPARE(session.publicProfile(),identity);QCOMPARE(controller.ownStatusContextKey(),context);
            QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
            QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("💬"));
            QVERIFY(current("ownStatusText")&&current("ownStatusEmojiDraft"));
            QCOMPARE(current("ownStatusText")->property("text").toString(),QString("Before"));
            QCOMPARE(current("ownStatusEmojiDraft")->property("text").toString(),QString("💬"));
        }
        // Both raw phases are logged before the independent first oracle. Search
        // rows discriminate the global 16px input rule even when preview is RED.
        if(firstOracle=="search")for(const int size:searchSizes)QCOMPARE(size,16);
        for(qsizetype i=0;i<inkBounds.size();++i) {
            const auto &ink=inkBounds.at(i);const auto &box=emojiBoxes.at(i);const auto &roi=rois.at(i);
            QVERIFY(!ink.isEmpty());QVERIFY(ink.left()>roi.left()&&ink.right()<roi.right());
            QVERIFY(ink.top()>roi.top()&&ink.bottom()<roi.bottom());
            QVERIFY2(qAbs(ink.width()-48)<=1,"Source yellow preview ink is 48px wide (+/-1), not the 45px layout box");
            QVERIFY2(qAbs(ink.height()-(locale=="ru"?46:45))<=1,"Source measured yellow preview ink height (+/-1)");
            QVERIFY2(qAbs(ink.left()-(box.left()+4))<=1,"Source measured ink starts four pixels inside the left-aligned preview box (+/-1)");
            QVERIFY2(qAbs(ink.top()-(box.top()+(locale=="ru"?-1:0)))<=1,"Source measured preview ink top (+/-1)");
        }
        if(firstOracle=="preview")for(const int size:searchSizes)QCOMPARE(size,16);
        click(window,current("ownStatusCancel"));QTRY_VERIFY(!current("ownStatusText"));
        QCOMPARE(fixture.posts,0);QCOMPARE(controller.ownStatusContextKey(),context);
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }
    void ownStatusPickerHoverAndArrowKeepFocusOnTheActualEmojiButton_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<QString>("mode");
        QTest::newRow("en-light")<<QString("en")<<QString("light");
        QTest::newRow("ru-dark")<<QString("ru")<<QString("dark");
    }
    void ownStatusPickerHoverAndArrowKeepFocusOnTheActualEmojiButton()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);WindowOwnStatusFixture fixture;
        QTemporaryDir directory;QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("status-picker-button-focus.ini"));
        preferences.setLocale(locale);preferences.setMode(mode);
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Picker button focus teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!controller.ownStatusContextKey().isEmpty());
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("💬"));
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));
        click(window,visibleItem("ownStatusAction"));QTRY_VERIFY(visibleItem("ownStatusChooseEmoji"));
        click(window,visibleItem("ownStatusChooseEmoji"));QTRY_VERIFY(visibleItem("ownStatusEmojiPicker"));
        QPointer<QQuickItem> picker=visibleItem("ownStatusEmojiPicker");QVERIFY(picker);
        const auto current=[&](const QString &name) { return picker?visibleItem(picker.data(),name):nullptr; };
        QTRY_VERIFY(current("reactionSearch")&&current("reactionSearch")->hasActiveFocus());
        QTRY_VERIFY(current("emoji-choice-grinning")&&current("reactionGrid"));
        auto *first=current("emoji-choice-grinning");auto *grid=current("reactionGrid");QVERIFY(first&&grid);
        const auto firstInGrid=QRectF(first->mapToItem(grid,QPointF{}),first->size());
        QVERIFY(QRectF(QPointF{},grid->size()).contains(firstInGrid));
        const auto mousePoint=first->mapToScene(QPointF(first->width()/2,first->height()/2));
        QVERIFY(QRectF(QPointF{},QSizeF(window->size())).contains(mousePoint));
        QSignalSpy chosen(picker.data(),SIGNAL(chosen(QString)));QVERIFY(chosen.isValid());
        QTest::mouseMove(window,mousePoint.toPoint());
        QTRY_VERIFY(current("emoji-choice-grinning")&&current("emoji-choice-grinning")->property("hovered").toBool());
        QTRY_VERIFY(current("reactionPreviewLabel")&&current("reactionPreviewLabel")->property("text").toString()==QString("grinning face"));
        QCOMPARE(chosen.count(),0);QCOMPARE(fixture.posts,0);
        // First intended behavioral RED: Source's actual fully-visible hover
        // focuses BUTTON 1f600; a GridView/anonymous delegate focus is insufficient.
        QTRY_VERIFY(current("emoji-choice-grinning")&&current("emoji-choice-grinning")->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Right);
        QTRY_VERIFY(current("emoji-choice-grinning_face_with_big_eyes")
            &&current("emoji-choice-grinning_face_with_big_eyes")->hasActiveFocus());
        QTRY_VERIFY(current("reactionPreviewLabel")&&current("reactionPreviewLabel")->property("text").toString()==QString("grinning face with big eyes"));
        QCOMPARE(chosen.count(),0);QCOMPARE(fixture.posts,0);
        QTest::keyClick(window,Qt::Key_Return);
        QTRY_COMPARE(chosen.count(),1);
        QCOMPARE(chosen.first().first().toString(),QString("grinning_face_with_big_eyes"));
        QTRY_VERIFY(!visibleItem("ownStatusEmojiPicker"));
        QTRY_VERIFY(visibleItem("ownStatusEmojiDraft")&&visibleItem("ownStatusEmojiDraft")->property("text").toString()==QString("😃"));
        auto *statusText=visibleItem("ownStatusText");QVERIFY(statusText);
        QCOMPARE(statusText->property("text").toString(),QString("Before"));
        QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("💬"));QCOMPARE(fixture.posts,0);
        click(window,visibleItem("ownStatusCancel"));QTRY_VERIFY(!visibleItem("ownStatusText"));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }
    void widenedPickerKeepsEveryEmojiBelowTheCategoryHeading_data()
    {
        QTest::addColumn<int>("pickerWidth");
        QTest::newRow("actual-old-status-width-408") << 408;
        QTest::newRow("matched-source-inner-width-404") << 404;
    }
    void widenedPickerKeepsEveryEmojiBelowTheCategoryHeading()
    {
        QFETCH(int,pickerWidth);
        QQmlEngine engine;engine.rootContext()->setContextProperty("testCatalog",knownReactionEmojis());
        QQmlComponent component(&engine);
        component.setData(R"QML(import QtQuick
            import "qrc:/workspace/qml"
            NativeReactionPicker { width: 404; height: 320; choices: testCatalog; active: true; skinTonesEnabled: false }
        )QML",QUrl("qrc:/native-status-picker-responsive-test.qml"));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QQuickWindow window;QScopedPointer<QQuickItem> picker(qobject_cast<QQuickItem *>(component.create()));QVERIFY(picker);
        picker->setWidth(pickerWidth);window.resize(460,380);picker->setParentItem(window.contentItem());picker->setPosition({20,20});
        auto close=qScopeGuard([&]{const bool closed=closeObservedTestWindow(&window);picker->setParentItem(nullptr);if(!closed)QTest::qFail("Picker component focus teardown",__FILE__,__LINE__);});
        window.show();window.requestActivate();QVERIFY(QTest::qWaitForWindowExposed(&window));QVERIFY(QTest::qWaitForWindowActive(&window));
        QTRY_VERIFY(item(picker.get(),"emoji-choice-grinning"));
        auto *heading=item(picker.get(),"reaction-heading-smileys_people");QVERIFY(heading);
        auto *first=item(picker.get(),"emoji-choice-grinning");QVERIFY(first);
        const auto bottom=heading->mapToItem(picker.get(),{0,heading->height()}).y();
        // Existing public component at the actual widened size. No new API is needed
        // to reproduce the painted heading/emoji overlap on the old product.
        QVERIFY2(first->mapToItem(picker.get(),QPointF{}).y()>=bottom,"First emoji paints inside the category heading row");
        for(const auto *name:{"grinning","grinning_face_with_big_eyes","grinning_face_with_closed_eyes","grin","laughing","grinning_face_with_sweat","rofl","joy","slightly_smiling_face"}){
            auto *emoji=item(picker.get(),"emoji-choice-"+QString::fromLatin1(name));QVERIFY(emoji);
            QCOMPARE(emoji->mapToItem(picker.get(),QPointF{}).y(),bottom);QCOMPARE(emoji->size(),QSizeF(40,40));
        }
        auto *next=item(picker.get(),"emoji-choice-upside_down_face");QVERIFY(next);
        QCOMPARE(next->mapToItem(picker.get(),QPointF{}).y(),bottom+40);
        QCOMPARE(next->mapToItem(picker.get(),QPointF{}).x(),first->mapToItem(picker.get(),QPointF{}).x());
        // Real public size change must reflow without stale heading/spacer strides.
        const auto firstPosition=[&]{auto *current=item(picker.get(),"emoji-choice-grinning");return current?current->mapToItem(picker.get(),QPointF{}):QPointF(-1,-1);};
        picker->setWidth(318);QTRY_COMPARE(firstPosition(),QPointF(11,171));
        picker->setWidth(pickerWidth);QTRY_COMPARE(firstPosition().y(),bottom);
    }

    void widenedPickerKeyboardDownUsesTheActualNineColumnRow()
    {
        QQmlEngine engine;engine.rootContext()->setContextProperty("testCatalog",knownReactionEmojis());
        QQmlComponent component(&engine);
        component.setData(R"QML(import QtQuick
            import "qrc:/workspace/qml"
            NativeReactionPicker { width: 404; height: 320; choices: testCatalog; active: true; skinTonesEnabled: false }
        )QML",QUrl("qrc:/native-status-picker-keyboard-test.qml"));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QQuickWindow window;QScopedPointer<QQuickItem> picker(qobject_cast<QQuickItem *>(component.create()));QVERIFY(picker);
        window.resize(460,380);picker->setParentItem(window.contentItem());picker->setPosition({20,20});
        auto close=qScopeGuard([&]{const bool closed=closeObservedTestWindow(&window);picker->setParentItem(nullptr);if(!closed)QTest::qFail("Picker component focus teardown",__FILE__,__LINE__);});
        window.show();window.requestActivate();QVERIFY(QTest::qWaitForWindowExposed(&window));QVERIFY(QTest::qWaitForWindowActive(&window));
        QTRY_VERIFY(item(picker.get(),"reactionSearch"));click(&window,item(picker.get(),"reactionSearch"));
        QSignalSpy chosen(picker.get(),SIGNAL(chosen(QString)));QVERIFY(chosen.isValid());
        QTest::keyClick(&window,Qt::Key_Down);QTest::keyClick(&window,Qt::Key_Down);QTest::keyClick(&window,Qt::Key_Return);
        QTRY_COMPARE(chosen.count(),1);
        QCOMPARE(chosen.first().first().toString(),QString("upside_down_face"));
    }
    void optionalPickerPreviewMatchesSourceDefaultsAndRealHover_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;QTest::newRow("dark") << true;
    }
    void optionalPickerPreviewMatchesSourceDefaultsAndRealHover()
    {
        QFETCH(bool,dark);
        QQmlEngine engine;engine.rootContext()->setContextProperty("testCatalog",knownReactionEmojis());
        QQmlComponent component(&engine);
        component.setData(R"QML(import QtQuick
            import "qrc:/workspace/qml"
            NativeReactionPicker { width: 404; height: 320; choices: testCatalog; active: true; skinTonesEnabled: false }
        )QML",QUrl("qrc:/native-status-picker-preview-test.qml"));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QQuickWindow window;QScopedPointer<QQuickItem> picker(qobject_cast<QQuickItem *>(component.create()));QVERIFY(picker);
        window.resize(460,380);picker->setParentItem(window.contentItem());picker->setPosition({20,20});
        auto close=qScopeGuard([&]{const bool closed=closeObservedTestWindow(&window);picker->setParentItem(nullptr);if(!closed)QTest::qFail("Picker component focus teardown",__FILE__,__LINE__);});
        // This is the new public optional-config contract. An absent property is
        // a missing capability, not the heading-row behavioral reproduction.
        QVERIFY(picker->metaObject()->indexOfProperty("previewEnabled")>=0);
        QVERIFY(!picker->property("previewEnabled").toBool());QVERIFY(picker->setProperty("dark",dark));
        window.show();window.requestActivate();QVERIFY(QTest::qWaitForWindowExposed(&window));QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::mouseMove(&window,{5,5});
        QVERIFY(!visibleItem(picker.get(),"reactionPreview"));
        auto *unexpandedGrid=item(picker.get(),"reactionGrid");QVERIFY(unexpandedGrid);QCOMPARE(unexpandedGrid->size(),QSizeF(382,189));
        QVERIFY(picker->setProperty("previewEnabled",true));
        QTRY_VERIFY(visibleItem(picker.get(),"reactionPreview"));
        auto *preview=visibleItem(picker.get(),"reactionPreview");auto *icon=visibleItem(picker.get(),"reactionPreviewEmoji");auto *label=visibleItem(picker.get(),"reactionPreviewLabel");
        QVERIFY(preview&&icon&&label);QCOMPARE(preview->mapToItem(picker.get(),QPointF{}),QPointF(1,249));QCOMPARE(preview->size(),QSizeF(402,70));
        QCOMPARE(icon->mapToItem(preview,QPointF{}),QPointF(10,13));QCOMPARE(icon->size(),QSizeF(45,45));
        QCOMPARE(icon->property("text").toString(),QString("😊"));QCOMPARE(label->property("text").toString(),QString("What's your mood?"));
        QCOMPARE(label->property("font").value<QFont>().pixelSize(),14);QCOMPARE(label->property("font").value<QFont>().capitalization(),QFont::Capitalize);
        auto *grid=item(picker.get(),"reactionGrid");QVERIFY(grid);QCOMPARE(grid->size(),QSizeF(367,118));
        auto *first=item(picker.get(),"emoji-choice-grinning");QVERIFY(first);QCOMPARE(first->mapToItem(picker.get(),QPointF{}),QPointF(11,171));
        auto *ninth=item(picker.get(),"emoji-choice-slightly_smiling_face");QVERIFY(ninth);QCOMPARE(ninth->mapToItem(picker.get(),QPointF{}).x(),qreal(331));
        QSignalSpy chosen(picker.get(),SIGNAL(chosen(QString)));QVERIFY(chosen.isValid());
        QTest::mouseMove(&window,first->mapToScene({20,20}).toPoint());
        QTRY_COMPARE(icon->property("text").toString(),QString("😀"));QTRY_COMPARE(label->property("text").toString(),QString("grinning face"));QCOMPARE(chosen.count(),0);
        QTest::mouseMove(&window,{5,5});QTRY_COMPARE(icon->property("text").toString(),QString("😊"));QTRY_COMPARE(label->property("text").toString(),QString("What's your mood?"));
        click(&window,item(picker.get(),"reactionSearch"));QTest::keyClick(&window,Qt::Key_Down);
        QTRY_COMPARE(label->property("text").toString(),QString("grinning face"));
        QTest::keyClick(&window,Qt::Key_Escape);QTRY_COMPARE(label->property("text").toString(),QString("What's your mood?"));QCOMPARE(chosen.count(),0);
        QVERIFY(picker->setProperty("previewEnabled",false));QTRY_VERIFY(!visibleItem(picker.get(),"reactionPreview"));QCOMPARE(grid->width(),qreal(382));
    }
    void ownStatusPickerAcceptsImmediateTypingWithoutAnotherSearchClick_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<QString>("mode");
        QTest::newRow("en-light")<<QString("en")<<QString("light");QTest::newRow("ru-dark")<<QString("ru")<<QString("dark");
    }
    void ownStatusPickerAcceptsImmediateTypingWithoutAnotherSearchClick()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);WindowOwnStatusFixture fixture;
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("status-picker-keyboard.ini"));preferences.setLocale(locale);preferences.setMode(mode);
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Status picker keyboard teardown",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!controller.ownStatusContextKey().isEmpty());QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));click(window,visibleItem("ownStatusAction"));
        QTRY_VERIFY(visibleItem("ownStatusChooseEmoji"));click(window,visibleItem("ownStatusChooseEmoji"));QTRY_VERIFY(visibleItem("ownStatusEmojiPicker"));
        QPointer<QQuickItem> picker=visibleItem("ownStatusEmojiPicker");QVERIFY(picker);
        QPointer<QQuickItem> search=item(picker.data(),"reactionSearch");QVERIFY(search);
        // Real immediate keyboard, deliberately no click/focusSearch invocation here.
        for(const auto key:QByteArray("coffee"))QTest::keyClick(window,key);
        QTRY_COMPARE(search->property("text").toString(),QString("coffee"));QVERIFY(search->hasActiveFocus());
        QTRY_VERIFY(visibleItem(picker.data(),"emoji-choice-coffee"));click(window,visibleItem(picker.data(),"emoji-choice-coffee"));
        QTRY_VERIFY(!visibleItem("ownStatusEmojiPicker"));QCOMPARE(visibleItem("ownStatusEmojiDraft")->property("text").toString(),QString("☕"));
        QCOMPARE(visibleItem("ownStatusText")->property("text").toString(),QString("Before"));QCOMPARE(fixture.posts,0);
        click(window,visibleItem("ownStatusCancel"));QTRY_VERIFY(!visibleItem("ownStatusText"));QCOMPARE(fixture.posts,0);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }
    void ownStatusUsesMeasuredSourceGeometry_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("firstOracle");
        for(const auto &entry:{QString("box"),QString("input"),QString("chooser"),QString("picker")}) {
            const auto en=("en-light-"+entry).toUtf8(),ru=("ru-dark-"+entry).toUtf8();
            QTest::newRow(en.constData())<<QString("en")<<QString("light")<<entry;
            QTest::newRow(ru.constData())<<QString("ru")<<QString("dark")<<entry;
        }
    }
    void ownStatusUsesMeasuredSourceGeometry()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);QFETCH(QString,firstOracle);
        WindowOwnStatusFixture fixture;fixture.held=true;
        QTemporaryDir directory;QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("status-geometry.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Status geometry teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        preferences.setLocale(locale);preferences.setMode(mode);
        QCOMPARE(preferences.paletteId(),QString("blue-cold"));
        QVERIFY(!controller.ownStatusContextKey().isEmpty());
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("💬"));
        QVERIFY(!controller.ownStatus().value("away").toBool());
        QCOMPARE(controller.account().value("displayEmail").toString(),QString("cassi@example.invalid"));
        const auto profile=controller.account(),identity=session.publicProfile();
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));
        click(window,visibleItem("ownStatusAction"));QTRY_VERIFY(visibleItem("ownStatusText"));
        auto *dialog=window->findChild<QObject *>("ownStatusDialog");QVERIFY(dialog&&dialog->property("visible").toBool());
        QSignalSpy frame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!frame.isEmpty());
        QCOMPARE(window->size(),QSize(1440,900));QCOMPARE(window->devicePixelRatio(),qreal(1));
        auto *input=visibleItem("ownStatusText"),*away=visibleItem("ownStatusAway");QVERIFY(input&&away);
        auto *content=dialog->property("contentItem").value<QQuickItem *>();QVERIFY(content);
        const auto rect=[](QQuickItem *entry){return QRectF(entry->mapToScene(QPointF{}),entry->size());};
        const auto near=[](qreal a,qreal b){return qAbs(a-b)<=0.01;};
        // #16: the fixed-height 20 px description line wraps on the resolved font's advances.
        // Derive the wrap extra from the measured lineCount so the geometry oracles stay exact
        // on every host font stack instead of pinning one machine's metrics.
        auto *statusDescription=static_cast<QQuickItem *>(nullptr);
        std::function<void(QQuickItem *)> collectDescription=[&](QQuickItem *entry){
            if(entry->isVisible()&&entry->property("lineHeight").toReal()==qreal(20)
               &&(!statusDescription
               ||entry->property("text").toString().size()>statusDescription->property("text").toString().size()))
                statusDescription=entry;
            for(auto *child:entry->childItems())collectDescription(child);
        };
        collectDescription(content);
        QVERIFY(statusDescription);
        QVERIFY(statusDescription->property("text").toString().size()>40);
        const auto statusWrapExtra=qreal((statusDescription->property("lineCount").toInt()-1)*20);
        QVERIFY(statusWrapExtra>=0);
        const auto titleText=locale=="ru"?QString("Статус"):QString("Status");
        QList<QQuickItem *> labels;
        std::function<void(QQuickItem *)> collect=[&](QQuickItem *entry){
            if(entry->isVisible()&&entry->property("text").toString()==titleText)labels.append(entry);
            for(auto *child:entry->childItems())collect(child);
        };
        collect(content);
        QCOMPARE(labels.size(),2);
        if(rect(labels.first()).top()>rect(labels.last()).top())labels.swapItemsAt(0,1);
        auto *title=labels.first(),*fieldLabel=labels.last();
        auto *background=dialog->property("background").value<QQuickItem *>();QVERIFY(background);
        auto *choose=visibleItem("ownStatusChooseEmoji");QVERIFY(choose);
        if(firstOracle=="box") {
            // Actual six Source frames: normal/held 448x332 at y284, not a mockup.
            QCOMPARE(dialog->property("height").toReal(),qreal(332)+statusWrapExtra); // +20 per extra description wrap line (#16)
            QCOMPARE(dialog->property("width").toReal(),qreal(448));
            QCOMPARE(dialog->property("x").toReal(),qreal(496));QCOMPARE(dialog->property("y").toReal(),qreal(284)-statusWrapExtra/2);
            QCOMPARE(background->property("radius").toReal(),qreal(10));
            QCOMPARE(dialog->property("padding").toReal(),qreal(1));
            QCOMPARE(content->width(),qreal(446));
            QCOMPARE(title->height(),qreal(24));QCOMPARE(title->property("lineHeight").toReal(),qreal(24));
            QCOMPARE(title->property("font").value<QFont>().pixelSize(),16);
            QVERIFY(near(rect(title).left(),517)&&near(rect(title).top(),301-statusWrapExtra/2));
            QVERIFY(near(rect(fieldLabel).top(),434+statusWrapExtra/2));QCOMPARE(fieldLabel->height(),qreal(20));
            QCOMPARE(fixture.posts,0);
        } else if(firstOracle=="input") {
            // Pinned global input max(16px,1em) wins over local text-sm14.
            QCOMPARE(input->property("font").value<QFont>().pixelSize(),16); // old14 intended RED
            auto *field=input->parentItem()->parentItem();QVERIFY(field);
            QCOMPARE(field->size(),QSizeF(406,42));
            QCOMPARE(input->property("leftPadding").toReal(),qreal(0));
            QCOMPARE(input->property("rightPadding").toReal(),qreal(0));
            QVERIFY(near(rect(input).left(),558));QCOMPARE(input->width(),qreal(352));
            // CSS layout is 22.84375px; Qt Layout can round its 22.8571 hint.
            QVERIFY(qAbs(input->height()-qreal(22.84375))<=0.25);
            auto *indicator=away->property("indicator").value<QQuickItem *>();QVERIFY(indicator);
            QCOMPARE(indicator->size(),QSizeF(16,16));
            auto *awayText=away->property("contentItem").value<QQuickItem *>();QVERIFY(awayText);
            QCOMPARE(awayText->property("leftPadding").toReal(),qreal(24));
            QCOMPARE(awayText->property("lineHeight").toReal(),qreal(20));
            for(const auto *id:{"ownStatusCancel","ownStatusSave"}) {
                auto *button=visibleItem(id);QVERIFY(button);
                QCOMPARE(button->property("leftPadding").toReal(),qreal(16));
                QCOMPARE(button->property("rightPadding").toReal(),qreal(16));
                QCOMPARE(button->property("topPadding").toReal(),qreal(8));
                QCOMPARE(button->height(),qreal(36));
                auto *paint=button->property("background").value<QQuickItem *>();QVERIFY(paint);
                QCOMPARE(paint->property("radius").toReal(),qreal(6));
            }
            auto *clear=visibleItem("ownStatusClear");QVERIFY(clear);
            QCOMPARE(clear->property("leftPadding").toReal(),qreal(12));
        } else if(firstOracle=="chooser") {
            QCOMPARE(int(choose->property("font").value<QFont>().weight()),int(QFont::Medium)); // old400 RED
            auto *paint=choose->property("background").value<QQuickItem *>();QVERIFY(paint);
            QCOMPARE(paint->property("radius").toReal(),qreal(6));
            QCOMPARE(QQmlProperty(paint,"border.width").read().toReal(),qreal(1));
            auto *label=choose->property("contentItem").value<QQuickItem *>();QVERIFY(label);
            QCOMPARE(label->property("font").value<QFont>().pixelSize(),12);
            QCOMPARE(label->property("lineHeight").toReal(),qreal(16));
            QCOMPARE(label->property("color").value<QColor>(),preferences.dark()?QColor("#ffffff"):QColor("#1b1b1d"));
            QVERIFY(near(choose->width()-label->implicitWidth(),22)); // border2 + horizontal padding20
            QVERIFY(!choose->property("hovered").toBool());
            const auto expected=preferences.dark()?QColor("#141517"):QColor("#e4ecf3");
            QCOMPARE(paint->property("color").value<QColor>(),expected);
            const auto pixels=window->grabWindow();QVERIFY(!pixels.isNull());
            const auto point=choose->mapToScene(QPointF(4,choose->height()/2)).toPoint();
            QVERIFY(pixels.rect().contains(point));const auto actual=pixels.pixelColor(point);
            QVERIFY(qAbs(actual.red()-expected.red())<=3&&qAbs(actual.green()-expected.green())<=3&&qAbs(actual.blue()-expected.blue())<=3);
            click(window,choose);QTRY_VERIFY(visibleItem("ownStatusEmojiPicker"));
            choose=visibleItem("ownStatusChooseEmoji");QVERIFY(choose);
            paint=choose->property("background").value<QQuickItem *>();QVERIFY(paint);
            const auto selected=paint->property("color").value<QColor>();
            QVERIFY(qAbs(selected.alphaF()-0.15)<=1.0/255);
            QCOMPARE(QQmlProperty(paint,"border.color").read().value<QColor>(),QColor("#7087ff"));
            click(window,choose);QTRY_VERIFY(!visibleItem("ownStatusEmojiPicker"));
        } else {
            QCOMPARE(firstOracle,QString("picker"));
            click(window,choose);QTRY_VERIFY(visibleItem("ownStatusEmojiPicker"));
            QSignalSpy pickerFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!pickerFrame.isEmpty());
            auto *picker=visibleItem("ownStatusEmojiPicker");QVERIFY(picker);
            QCOMPARE(dialog->property("height").toReal(),qreal(670)+statusWrapExtra); // +20 per extra description wrap line (#16)
            QCOMPARE(dialog->property("y").toReal(),qreal(115)-statusWrapExtra/2);
            QCOMPARE(picker->size(),QSizeF(404,320));
            QVERIFY(near(rect(picker).left(),518)&&near(rect(picker).top(),266+statusWrapExtra/2));
            auto *wrapper=picker->parentItem()->parentItem();QVERIFY(wrapper);
            QCOMPARE(wrapper->size(),QSizeF(406,322));
            QCOMPARE(wrapper->property("radius").toReal(),qreal(8));
            QCOMPARE(QQmlProperty(wrapper,"border.width").read().toReal(),qreal(1));
            QVERIFY(!visibleItem(picker,"reactionSkinToneToggle"));QCOMPARE(fixture.posts,0);
            click(window,visibleItem("ownStatusChooseEmoji"));QTRY_VERIFY(!visibleItem("ownStatusEmojiPicker"));
        }
        // Same actual held HTTP stage as existing functional tests; no fake busy.
        click(window,visibleItem("ownStatusPreset-1"));
        click(window,visibleItem("ownStatusText"));QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const auto key:QByteArray("  Focusing  "))QTest::keyClick(window,key);
        click(window,visibleItem("ownStatusAway"));click(window,visibleItem("ownStatusSave"));
        QTRY_COMPARE(fixture.posts,1);QTRY_VERIFY(bool(fixture.release));QTRY_VERIFY(fixture.pending(api));
        QCOMPARE(fixture.posted,QJsonObject({{"status","idle"},{"emoji","🏠"},{"text","Focusing"}}));
        QVERIFY(controller.ownStatus().value("pending").toBool());QCOMPARE(controller.account(),profile);
        QSignalSpy heldFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!heldFrame.isEmpty());
        QCOMPARE(dialog->property("height").toReal(),qreal(332)+statusWrapExtra);
        for(const auto *id:{"ownStatusSave","ownStatusCancel","ownStatusClear"}) {
            auto *button=visibleItem(id);QVERIFY(button&&!button->isEnabled());QCOMPARE(button->height(),qreal(36));
        }
        QVERIFY(visibleItem("ownStatusText")->isEnabled());QVERIFY(visibleItem("ownStatusAway")->isEnabled());
        QVERIFY(visibleItem("ownStatusPreset-1")->isEnabled());
        auto send=std::move(fixture.release);fixture.release={};send();
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_COMPARE(controller.ownStatus().value("text").toString(),QString("Focusing"));
        QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("🏠"));QVERIFY(controller.ownStatus().value("away").toBool());
        QCOMPARE(session.publicProfile(),identity);QCOMPARE(fixture.posts,1);QCOMPARE(fixture.usersGets,1);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }
    void ownStatusActualMainSavesOneHeldCanonicalResponse_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<QString>("mode");
        QTest::newRow("en-light")<<QString("en")<<QString("light");
        QTest::newRow("ru-dark")<<QString("ru")<<QString("dark");
    }
    void ownStatusActualMainSavesOneHeldCanonicalResponse()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);
        WindowOwnStatusFixture fixture;fixture.held=true;
        QTemporaryDir directory;QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("status-ui.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Own status window did not settle",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        preferences.setLocale(locale);preferences.setMode(mode);
        // Mandatory actual C++ capability prerequisites; failure here is NOT missing-editor UI RED.
        QVERIFY(!controller.ownStatusContextKey().isEmpty());
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("💬"));
        QVERIFY(!controller.ownStatus().value("away").toBool());
        QCOMPARE(controller.account().value("displayEmail").toString(),QString("cassi@example.invalid"));
        const auto identity=session.publicProfile(),profile=controller.account();
        click(window,visibleItem("accountMenuButton"));
        QTRY_VERIFY(visibleItem("ownStatusAction")); // First intended old-Main missing-editor RED.
        auto *statusAction = visibleItem(window->contentItem(), "ownStatusAction");
        auto *aboutAction = visibleItem(window->contentItem(), "aboutAction");
        QVERIFY(statusAction && aboutAction);
        QQmlExpression statusPopupExpression(qmlContext(window), window, QStringLiteral("accountPopup"));
        const auto statusPopupValue = statusPopupExpression.evaluate();
        auto *statusPopup = statusPopupValue.metaType() == QMetaType::fromType<QJSValue>()
            ? statusPopupValue.value<QJSValue>().toQObject() : statusPopupValue.value<QObject *>();
        QVERIFY(statusPopup && !statusPopupExpression.hasError());
        const auto statusActionReady = [&] {
            auto *column = statusAction->parentItem();
            if (!column || aboutAction->parentItem() != column || !statusAction->isVisible()
                || !statusAction->isEnabled() || statusAction->window() != window
                || !statusPopup->property("visible").toBool() || !statusPopup->property("opened").toBool()
                || column->height() != column->implicitHeight()
                || statusAction->height() != statusAction->implicitHeight()) return false;
            const QRectF statusRect(statusAction->mapToScene(QPointF{}), statusAction->size());
            const QRectF aboutRect(aboutAction->mapToScene(QPointF{}), aboutAction->size());
            const QRectF columnRect(column->mapToScene(QPointF{}), column->size());
            return statusRect.width() > 0 && statusRect.height() > 0 && columnRect.contains(statusRect)
                && columnRect.contains(aboutRect) && aboutRect.top() >= statusRect.bottom();
        };
        QTRY_VERIFY2(statusActionReady(), "The account menu must finish placing its rows before mouse input");
        QSignalSpy statusInputFrame(window, &QQuickWindow::frameSwapped);
        window->update(); QTRY_VERIFY(!statusInputFrame.isEmpty());
        QVERIFY(statusActionReady());
        QSignalSpy statusClicked(statusAction, SIGNAL(clicked())); QVERIFY(statusClicked.isValid());
        click(window,statusAction); QCOMPARE(statusClicked.size(),1);
        QTRY_VERIFY(visibleItem("ownStatusText"));
        auto *dialog=window->findChild<QObject *>("ownStatusDialog");QVERIFY(dialog&&dialog->property("visible").toBool());
        QCOMPARE(visibleItem("ownStatusText")->property("text").toString(),QString("Before"));
        QCOMPARE(visibleItem("ownStatusSave")->property("text").toString(),locale=="ru"?QString("Сохранить"):QString("Save"));
        const auto inside=[&](QQuickItem *entry){
            if(!entry||!entry->isVisible())return false;
            return QRectF(0,0,window->width(),window->height()).contains(QRectF(entry->mapToScene(QPointF{}),QSizeF(entry->width(),entry->height())));
        };
        QVERIFY(inside(visibleItem("ownStatusSave")));QVERIFY(inside(visibleItem("ownStatusCancel")));
        click(window,visibleItem("ownStatusPreset-1"));
        QCOMPARE(visibleItem("ownStatusEmojiDraft")->property("text").toString(),QString("🏠"));
        click(window,visibleItem("ownStatusText"));QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const auto key:QByteArray("  Focusing  "))QTest::keyClick(window,key);
        click(window,visibleItem("ownStatusAway"));
        QVERIFY(visibleItem("ownStatusAway")->property("checked").toBool());
        click(window,visibleItem("ownStatusSave"));
        QTRY_COMPARE(fixture.posts,1);QTRY_VERIFY(bool(fixture.release));QTRY_VERIFY(fixture.pending(api));
        QCOMPARE(fixture.posted,QJsonObject({{"status","idle"},{"emoji","🏠"},{"text","Focusing"}}));
        QVERIFY(controller.ownStatus().value("pending").toBool());QCOMPARE(controller.account(),profile);
        for(const auto *id:{"ownStatusSave","ownStatusCancel","ownStatusClear"}) {
            auto *button=visibleItem(id);QVERIFY(button&&!button->isEnabled());
            QTest::mouseClick(window,Qt::LeftButton,{},button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint());
        }
        QTest::keyClick(window,Qt::Key_Escape);QVERIFY(dialog->property("visible").toBool());QCOMPARE(fixture.posts,1);
        auto send=std::move(fixture.release);fixture.release={};send();
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_COMPARE(controller.ownStatus().value("text").toString(),QString("Focusing"));
        QCOMPARE(controller.ownStatus().value("emoji").toString(),QString("🏠"));
        QVERIFY(controller.ownStatus().value("away").toBool());
        const auto savedLabel=[&] {auto *label=visibleItem("profilePresenceStatus");return label&&label->property("text").toString()=="🏠 Focusing";};
        QTRY_VERIFY(savedLabel());
        QCOMPARE(session.publicProfile(),identity);QCOMPARE(controller.account().value("email"),profile.value("email"));
        QCOMPARE(fixture.posts,1);QCOMPARE(fixture.usersGets,1);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }

    void ownStatusActualMainClearCancelPickerAndFailurePreserveTheDraft()
    {
        WindowOwnStatusFixture fixture;fixture.status=503;
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("status-failure.ini"));preferences.setLocale("en");
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Status failure window teardown",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!controller.ownStatusContextKey().isEmpty());
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        const auto profile=controller.account();
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));click(window,visibleItem("ownStatusAction"));
        QTRY_VERIFY(visibleItem("ownStatusText"));click(window,visibleItem("ownStatusClear"));
        QVERIFY(visibleItem("ownStatusText")->property("text").toString().isEmpty());
        QCOMPARE(visibleItem("ownStatusEmojiDraft")->property("text").toString(),QString("🙂"));
        QVERIFY(!visibleItem("ownStatusAway")->property("checked").toBool());QCOMPARE(fixture.posts,0);
        click(window,visibleItem("ownStatusCancel"));QTRY_VERIFY(!visibleItem("ownStatusText"));QCOMPARE(controller.account(),profile);
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));click(window,visibleItem("ownStatusAction"));QTRY_VERIFY(visibleItem("ownStatusText"));
        QCOMPARE(visibleItem("ownStatusText")->property("text").toString(),QString("Before"));
        click(window,visibleItem("ownStatusChooseEmoji"));QTRY_VERIFY(visibleItem("ownStatusEmojiPicker"));
        auto *picker=visibleItem("ownStatusEmojiPicker");QVERIFY(picker);
        QVERIFY(!visibleItem(picker,"reactionSkinToneToggle"));
        auto *search=visibleItem(picker,"reactionSearch");QVERIFY(search);click(window,search);
        for(const auto key:QByteArray("coffee"))QTest::keyClick(window,key);
        QTRY_VERIFY(visibleItem(picker,"emoji-choice-coffee"));click(window,visibleItem(picker,"emoji-choice-coffee"));
        QTRY_VERIFY(!visibleItem("ownStatusEmojiPicker"));
        QCOMPARE(visibleItem("ownStatusEmojiDraft")->property("text").toString(),QString("☕"));QCOMPARE(fixture.posts,0);
        click(window,visibleItem("ownStatusText"));QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const auto key:QByteArray("Retained draft"))QTest::keyClick(window,key);
        click(window,visibleItem("ownStatusSave"));QTRY_COMPARE(fixture.posts,1);
        QTRY_VERIFY(!controller.ownStatus().value("pending").toBool());QTRY_VERIFY(visibleItem("ownStatusErrorText"));
        QCOMPARE(visibleItem("ownStatusErrorText")->property("text").toString(),QString("Failed to update status"));
        QCOMPARE(visibleItem("ownStatusText")->property("text").toString(),QString("Retained draft"));
        QCOMPARE(visibleItem("ownStatusEmojiDraft")->property("text").toString(),QString("☕"));
        for(const auto *id:{"ownStatusSave","ownStatusClear","ownStatusCancel"}){auto *button=visibleItem(id);QVERIFY(button&&button->isEnabled());}
        QCOMPARE(controller.account(),profile);QCOMPARE(fixture.posts,1);
        click(window,visibleItem("ownStatusDismissError"));QTRY_VERIFY(!visibleItem("ownStatusErrorText"));
        click(window,visibleItem("ownStatusCancel"));QTRY_VERIFY(!visibleItem("ownStatusText"));QCOMPARE(fixture.posts,1);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }

    void ownStatusActualMainOwnerLossClosesThePendingEditor()
    {
        WindowOwnStatusFixture fixture;fixture.held=true;
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("status-owner.ini"));preferences.setLocale("en");
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Status owner window teardown",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!controller.ownStatusContextKey().isEmpty());
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));click(window,visibleItem("ownStatusAction"));
        QTRY_VERIFY(visibleItem("ownStatusText"));click(window,visibleItem("ownStatusSave"));
        QTRY_COMPARE(fixture.posts,1);QTRY_VERIFY(bool(fixture.release));QTRY_VERIFY(fixture.pending(api));
        const auto pending=fixture.pending(api);QVERIFY(pending&&!pending->isFinished());
        controller.logout(); // Real public ownership/lifecycle action, not a QML/private readiness setter.
        QTRY_VERIFY(pending.isNull()||pending->isFinished());QTRY_VERIFY(!visibleItem("ownStatusText"));
        QVERIFY(controller.ownStatus().isEmpty());QTRY_VERIFY(visibleItem("serverInput"));
        auto send=std::move(fixture.release);fixture.release={};send();
        QVERIFY(!visibleItem("ownStatusText"));QVERIFY(!visibleItem("ownStatusErrorText"));QCOMPARE(fixture.posts,1);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }


    void ownStatusActualMainApiGenerationClosesAndClearsThePendingEditor()
    {
        WindowOwnStatusFixture fixture;fixture.held=true;
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("status-generation.ini"));preferences.setLocale("en");
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Status generation window teardown",__FILE__,__LINE__);});
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const auto context=controller.ownStatusContextKey();QVERIFY(!context.isEmpty());QVERIFY(controller.ownerKey().isEmpty());
        QCOMPARE(controller.ownStatus().value("text").toString(),QString("Before"));
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));click(window,visibleItem("ownStatusAction"));
        QTRY_VERIFY(visibleItem("ownStatusText"));
        QPointer<QQuickItem> input=visibleItem("ownStatusText");QVERIFY(input);
        click(window,input.data());QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        for(const auto key:QByteArray("Retired draft"))QTest::keyClick(window,key);
        click(window,visibleItem("ownStatusSave"));QTRY_COMPARE(fixture.posts,1);
        QTRY_VERIFY(bool(fixture.release));QTRY_VERIFY(fixture.pending(api));
        const auto pending=fixture.pending(api);QVERIFY(pending&&!pending->isFinished());
        const auto generation=api.generation();api.setAccessToken("synthetic-status-ui-new-generation");
        QVERIFY(api.generation()>generation);QVERIFY(controller.authenticated());
        QVERIFY(!controller.ownStatusContextKey().isEmpty());QVERIFY(controller.ownStatusContextKey()!=context);
        QTRY_VERIFY(pending.isNull()||pending->isFinished());QTRY_VERIFY(!visibleItem("ownStatusText"));
        QVERIFY(input);QVERIFY(input->property("text").toString().isEmpty());
        QVERIFY(!controller.ownStatus().value("pending").toBool());
        auto send=std::move(fixture.release);fixture.release={};send();
        QVERIFY(!visibleItem("ownStatusText"));QVERIFY(!visibleItem("ownStatusErrorText"));QCOMPARE(fixture.posts,1);
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("ownStatusAction"));click(window,visibleItem("ownStatusAction"));
        QTRY_VERIFY(visibleItem("ownStatusText"));
        QCOMPARE(visibleItem("ownStatusText")->property("text").toString(),QString("Before"));
        click(window,visibleItem("ownStatusCancel"));QTRY_VERIFY(!visibleItem("ownStatusText"));QCOMPARE(fixture.posts,1);
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }

    void profileAndTopbarUseMeasuredThreeLineGeometry_data()
    {
        QTest::addColumn<QString>("firstOracle");
        QTest::newRow("height")<<QString("height");
        QTest::newRow("email")<<QString("email");
    }

    void profileAndTopbarUseMeasuredThreeLineGeometry()
    {
        QFETCH(QString,firstOracle);
        // Root's unchanged Source43 EN-light actual observation, SHA bd031e91...
        // Source desktop line boxes, padding and border are geometric oracles;
        // platform glyph advances/AA are not asserted pixel-exact across engines.
        QMap<QString,int> metadataGets,binaryGets;
        const QMap<QString,QByteArray> images{{ViewerAvatarFile,avatarProbePng(false)},
                                              {PeerAvatarFile,avatarProbePng(true)}};
        WindowDirectFixture fixture;fixture.seed();
        fixture.created["encryption"]=false;fixture.created["current_encryption_key_uuid"]=QJsonValue::Null;
        fixture.users=QJsonArray{
            QJsonObject{{"uuid",userId},{"username","cassi"},{"first_name","Cassi"},{"last_name","Synthetic"},
                {"email","cassi@example.invalid"},{"avatar","urn:image:"+ViewerAvatarFile},
                {"source","iam"},{"status","offline"},{"last_ping_at","2026-01-02T10:00:00Z"},
                {"created_at","2026-01-02T10:00:00Z"},{"updated_at","2026-01-02T10:00:00Z"}},
            QJsonObject{{"uuid",channelPeer},{"username","alice"},{"first_name","Alice"},{"last_name","Synthetic"},
                {"email","alice@example.invalid"},{"avatar","urn:image:"+PeerAvatarFile},
                {"source","iam"},{"status","offline"},{"last_ping_at","2026-01-02T10:00:00Z"},
                {"created_at","2026-01-02T10:00:00Z"},{"updated_at","2026-01-02T10:00:00Z"}}};
        auto peerMessage=message("A public synthetic peer message keeps the actual avatar rail visible. "
                                 "The picture belongs to Alice Synthetic, not the current viewer.",messageId,channelDefault);
        peerMessage["stream_uuid"]=windowDirectId();peerMessage["author_uuid"]=channelPeer;
        peerMessage["user_uuid"]=userId;peerMessage["is_own"]=false;peerMessage["pinned"]=false;
        peerMessage["reactions"]=QJsonObject{};peerMessage["reaction_users"]=QJsonObject{};
        peerMessage["updated_at"]=peerMessage.value("created_at");
        const auto original=fixture.server.handler;
        fixture.server.handler=[&,original,peerMessage](const Request &request)->Response {
            const auto path=request.target.path();
            for(auto it=images.cbegin();it!=images.cend();++it) {
                const auto file="/api/workspace/v1/messenger/files/"+it.key();
                if(request.method=="GET"&&path==file){++metadataGets[it.key()];return {avatarProbeMetadata(it.key(),it.value())};}
                if(request.method=="GET"&&path==file+"/actions/download") {
                    ++binaryGets[it.key()];Response response;response.rawBody=it.value();response.rawContentType="image/png";return response;
                }
            }
            auto response=original(request);
            if(request.method=="GET"&&path.endsWith("/messages/")
                &&QUrlQuery(request.target).queryItemValue("stream_uuid")==windowDirectId())response.data=QJsonArray{peerMessage};
            return response;
        };
        QTemporaryDir directory;QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("profile-geometry.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        FileClient files(&api);controller.useFiles(&files);AvatarImages avatars(&api,&session,&files);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences,&avatars);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Measured profile window did not settle before teardown",__FILE__,__LINE__);
        });
        window->resize(1440,900);QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));QCOMPARE(window->devicePixelRatio(),qreal(1));
        session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(),2);QTRY_VERIFY(controller.channelMembersVerified());
        preferences.setLocale("en");preferences.setMode("light");
        QCOMPARE(preferences.locale(),QString("en"));QCOMPARE(preferences.mode(),QString("light"));
        QTRY_COMPARE(controller.account().value("presence").toString(),QString("offline"));
        QTRY_COMPARE(controller.account().value("displayEmail").toString(),QString("cassi@example.invalid"));
        QVERIFY(controller.account().value("email").toString().isEmpty());
        QTRY_VERIFY(visibleItem("stream-"+windowDirectId()));click(window,visibleItem("stream-"+windowDirectId()));
        QTRY_COMPARE(controller.selectedStream(),windowDirectId());QVERIFY(controller.selectedTopic().isEmpty());
        QTRY_VERIFY(!controller.busy());QTRY_COMPARE(controller.messages().size(),1);
        QTRY_COMPARE(controller.directPeer().value("displayName").toString(),QString("Alice Synthetic"));
        const auto face=[&](int index)->QQuickItem * {
            if(index==0)return visibleItem("profileAvatar");
            if(index==1)return avatarProbeCircle(visibleItem("stream-"+windowDirectId()),40);
            if(index==2)return avatarProbeCircle(visibleItem("directConversationHeader"),40);
            return visibleItem("message-avatar-"+messageId);
        };
        for(int index=0;index<4;++index)QTRY_VERIFY(avatarProbeMatchesPixels(window,face(index),index!=0));
        QTRY_COMPARE(avatars.stats().leases,4);QTRY_COMPARE(avatars.stats().pipelineJobs,0);
        for(const auto &uuid:{ViewerAvatarFile,PeerAvatarFile}){
            QCOMPARE(metadataGets.value(uuid),1);QCOMPARE(binaryGets.value(uuid),1);
        }
        const QStringList badgeNames{"presence-viewer","presence-stream-"+windowDirectId(),"presence-direct-header","presence-author-"+messageId};
        for(const auto &name:badgeNames)QTRY_VERIFY(visibleItem(name));
        QTRY_VERIFY(visibleItem("profileDisplayName")&&visibleItem("profilePresenceStatus")&&visibleItem("profileEmail"));
        QCOMPARE(visibleItem("profileDisplayName")->property("text").toString(),QString("Cassi Synthetic"));
        QCOMPARE(visibleItem("profilePresenceStatus")->property("text").toString(),QString("offline"));
        QCOMPARE(visibleItem("profileEmail")->property("text").toString(),QString("cassi@example.invalid"));
        QTRY_VERIFY(visibleItem("streamTopicPrompt"));QVERIFY(!visibleItem("encryptedReadStatus"));
        QSignalSpy measuredFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!measuredFrame.isEmpty());
        auto *bar=visibleItem("topBar"),*profile=visibleItem("accountMenuButton");QVERIFY(bar&&profile);
        // Passive one-shot observation BEFORE the first measured height oracle.
        // No event pump, input, wait, state mutation or geometry setter here.
        const auto observedItem=[&](QQuickItem *entry) {
            if(!entry)return QJsonObject{{"present",false}};
            const auto scene=entry->mapToScene(QPointF{});
            QJsonObject data{{"present",true},{"type",entry->metaObject()->className()},{"name",entry->objectName()},
                {"x",entry->x()},{"y",entry->y()},{"sceneX",scene.x()},{"sceneY",scene.y()},
                {"width",entry->width()},{"height",entry->height()},
                {"implicitWidth",entry->implicitWidth()},{"implicitHeight",entry->implicitHeight()},
                {"visible",entry->isVisible()},{"clip",entry->clip()}};
            for(const auto *key:{"text","asset","truncated","contentWidth","contentHeight","availableWidth","availableHeight",
                "implicitContentWidth","implicitContentHeight","leftPadding","rightPadding","topPadding","bottomPadding",
                "baselineOffset","lineHeight","lineHeightMode","elide","maximumLineCount"}) {
                const auto value=entry->property(key);if(value.isValid())data[key]=QJsonValue::fromVariant(value);
            }
            const auto fontProperty=entry->property("font");
            if(fontProperty.isValid()) {
                const auto font=fontProperty.value<QFont>();const QFontInfo info(font);
                data["requestedFont"]=QJsonObject{{"family",font.family()},{"pixelSize",font.pixelSize()},
                    {"pointSize",font.pointSizeF()},{"weight",int(font.weight())},{"bold",font.bold()},{"italic",font.italic()}};
                data["fontInfo"]=QJsonObject{{"family",info.family()},{"styleName",info.styleName()},
                    {"pixelSize",info.pixelSize()},{"pointSize",info.pointSizeF()},{"weight",info.weight()},
                    {"bold",info.bold()},{"italic",info.italic()},{"exactMatch",info.exactMatch()}};
            }
            QJsonObject layout;
            for(const auto *key:{"minimumWidth","preferredWidth","maximumWidth","fillWidth","alignment","minimumHeight","preferredHeight","maximumHeight","fillHeight"}) {
                const auto value=QQmlProperty(entry,QString("Layout.")+key,&engine).read();
                if(!value.isValid())continue;
                // Maximum layout bounds may be infinity; JSON never labels that
                // as a measured finite cap or silently converts it to null.
                const bool floating=value.metaType().id()==QMetaType::Double||value.metaType().id()==QMetaType::Float;
                layout[key]=floating&&!qIsFinite(value.toDouble())?QJsonValue(QString("nonfinite")):QJsonValue::fromVariant(value);
            }
            if(!layout.isEmpty())data["layout"]=layout;
            return data;
        };
        auto *observedName=visibleItem("profileDisplayName");QVERIFY(observedName);
        auto *observedColumn=observedName->parentItem();QVERIFY(observedColumn);
        auto *observedRow=observedColumn->parentItem();QVERIFY(observedRow);
        QJsonArray contentChildren;QQuickItem *observedChevron=nullptr;
        for(auto *child:observedRow->childItems()) {
            contentChildren.append(observedItem(child));
            if(child->property("asset").toString()=="chevron-down.svg")observedChevron=child;
        }
        QJsonObject profileGeometryObservation{{"stage","before-first-source-height-oracle"},{"firstOracle",firstOracle},{"synthetic",true},
            {"viewport",QJsonObject{{"width",window->width()},{"height",window->height()},{"dpr",window->devicePixelRatio()}}},
            {"bar",observedItem(bar)},{"profile",observedItem(profile)},
            {"name",observedItem(observedName)},{"status",observedItem(visibleItem("profilePresenceStatus"))},
            {"email",observedItem(visibleItem("profileEmail"))},{"column",observedItem(observedColumn)},
            {"contentRow",observedItem(observedRow)},{"contentChildren",contentChildren},{"chevron",observedItem(observedChevron)},
            {"fontInfoScope","QFontInfo resolved for each requested QFont; not per-glyph fallback or cross-platform raster acceptance"}};
        const auto observedBytes=QJsonDocument(profileGeometryObservation).toJson(QJsonDocument::Compact);
        qInfo().noquote()<<"PROFILE_GEOMETRY_OBSERVATION"<<observedBytes;
        const auto observationDirectory=qEnvironmentVariable("CASSI_NATIVE_AVATAR_SCREENSHOT_DIRECTORY");
        if(!observationDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(observationDirectory));
            const auto path=QDir(observationDirectory).filePath("native-profile-geometry-"+firstOracle+"-before-height-oracle.json");
            QVERIFY(!QFile::exists(path));QFile output(path);QVERIFY(output.open(QIODevice::WriteOnly));
            const auto body=QJsonDocument(profileGeometryObservation).toJson(QJsonDocument::Indented);
            QCOMPARE(output.write(body),qint64(body.size()));
        }
        // Independent email-first row reaches the actual visible elision even
        // while the unchanged old topbar still has its separate height defect.
        if(firstOracle=="email") {
            auto *email=visibleItem("profileEmail");QVERIFY(email);
            QVERIFY2(!email->property("truncated").toBool(),"Actual full Source email must not be elided with this natural-width fixture");
        }
        // First intended unchanged-v50 RED: actual65 vs Source76.5, after all
        // actual images/data/presence prerequisites above have passed.
        QCOMPARE(bar->height(),qreal(76.5));
        QCOMPARE(profile->height(),qreal(59.5));QCOMPARE(profile->mapToScene(QPointF{}).y(),qreal(8));
        QCOMPARE(profile->mapToScene({profile->width(),0}).x(),qreal(1432));
        QCOMPARE(QQmlProperty(profile,"padding").read().toReal(),qreal(6));
        QCOMPARE(face(0)->mapToScene(QPointF{}).y(),qreal(19.75));QCOMPARE(face(0)->size(),QSizeF(36,36));
        const QStringList rows{"profileDisplayName","profilePresenceStatus","profileEmail"};
        const QList<qreal> y{14,34,47.75},heights{20,13.75,13.75};
        for(qsizetype index=0;index<rows.size();++index){
            auto *text=visibleItem(rows[index]);QVERIFY(text);
            QCOMPARE(text->mapToScene(QPointF{}).y(),y[index]);QCOMPARE(text->height(),heights[index]);
            QCOMPARE(text->property("lineHeight").toReal(),heights[index]);
            QCOMPARE(text->mapToItem(profile,QPointF{}).x(),qreal(50)); // p6+avatar36+gap8.
            QVERIFY(qAbs(text->width()-text->implicitWidth())<=1); // Source natural width; allow native layout pixel rounding.
            QVERIFY(!text->property("truncated").toBool());
            QCOMPARE(text->property("textFormat").toInt(),0);
        }
        auto *column=visibleItem("profileDisplayName")->parentItem();QVERIFY(column);
        QCOMPARE(column->height(),qreal(47.5));
        // Compare the natural width contract using the ACTUAL Qt glyph advances,
        // not Chromium's190.109375px width as a cross-font exact-pixel oracle.
        const auto natural=qMax(visibleItem(rows[0])->implicitWidth(),qMax(visibleItem(rows[1])->implicitWidth(),visibleItem(rows[2])->implicitWidth()));
        QVERIFY(qAbs(column->width()-natural)<=1);QVERIFY(qAbs(profile->width()-(natural+80))<=1);
        auto *organization=visibleItemWithPrefix("sessionQuick-");QVERIFY(organization);
        QCOMPARE(organization->mapToScene(QPointF{}).x(),qreal(28));
        QCOMPARE(organization->mapToScene({0,organization->height()/2}).y(),qreal(37.75));
        QCOMPARE(organization->size(),QSizeF(40,40));
        QVERIFY(QRectF(QPointF{},bar->size()).contains(profile->mapRectToItem(bar,{QPointF{},profile->size()})));
        QVERIFY(!face(0)->clip());
        QCOMPARE(visibleItem("conversationHeader")->mapToScene(QPointF{}).y(),qreal(80.5));
        QCOMPARE(visibleItem("conversationHeader")->height(),qreal(56));
        QVERIFY(!controller.canSend());QVERIFY(!controller.canAttach());QCOMPARE(fixture.posts+fixture.memberPosts,0);
        // Canonical nullable-email is a Source-CSS-derived secondary control:
        // two lines fit inside the36px avatar+p12, so only THIS state stays65.
        // No Source screenshot of this secondary state is claimed.
        auto own=fixture.users[0].toObject();own["email"]=QJsonValue::Null;fixture.users[0]=own;
        const auto beforeMissingEmail=fixture.usersGets;controller.refresh();
        QTRY_VERIFY(fixture.usersGets>beforeMissingEmail);QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(controller.account().value("displayEmail").toString().isEmpty());
        QTRY_VERIFY(!visibleItem("profileEmail"));QTRY_COMPARE(visibleItem("topBar")->height(),qreal(65));
        QCOMPARE(visibleItem("accountMenuButton")->height(),qreal(48));
        QCOMPARE(visibleItem("accountMenuButton")->mapToScene(QPointF{}).y(),qreal(8));
        QCOMPARE(visibleItem("profilePresenceStatus")->property("text").toString(),QString("offline"));
        own["email"]="cassi@example.invalid";fixture.users[0]=own;
        const auto beforeRestore=fixture.usersGets;controller.refresh();
        QTRY_VERIFY(fixture.usersGets>beforeRestore);QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.account().value("displayEmail").toString(),QString("cassi@example.invalid"));
        QTRY_VERIFY(visibleItem("profileEmail"));QTRY_COMPARE(visibleItem("topBar")->height(),qreal(76.5));
        QCOMPARE(visibleItem("accountMenuButton")->height(),qreal(59.5));
        for(int index=0;index<4;++index)QTRY_VERIFY(avatarProbeMatchesPixels(window,face(index),index!=0));
        for(const auto &uuid:{ViewerAvatarFile,PeerAvatarFile}){
            QCOMPARE(metadataGets.value(uuid),1);QCOMPARE(binaryGets.value(uuid),1);
        }
        QCOMPARE(fixture.posts+fixture.memberPosts,0);
        const bool closed=closeIncubatedTestWindow(engine,window);
        if(closed)closeBeforeEngine.dismiss();QVERIFY(closed);
    }
    void actualUsersAndRealtimePresenceUpdateMainWithoutReloadingPhotos_data()
    {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("encryptedDirect");
        QTest::newRow("en-light-direct")<<QString("en")<<QString("light")<<false;
        QTest::newRow("ru-dark-protected-direct")<<QString("ru")<<QString("dark")<<true;
    }

    void actualUsersAndRealtimePresenceUpdateMainWithoutReloadingPhotos()
    {
        QFETCH(QString,locale); QFETCH(QString,mode); QFETCH(bool,encryptedDirect);
        QMap<QString,int> metadataGets,binaryGets;
        const QMap<QString,QByteArray> images{{ViewerAvatarFile,avatarProbePng(false)},
                                              {PeerAvatarFile,avatarProbePng(true)}};
        WindowDirectFixture fixture; fixture.seed();
        fixture.created["encryption"]=encryptedDirect;
        fixture.created["current_encryption_key_uuid"]=QJsonValue::Null;
        fixture.users=QJsonArray{
            QJsonObject{{"uuid",userId},{"username","cassi"},{"first_name","Cassi"},{"last_name","Synthetic"},
                {"email","cassi@example.invalid"},{"avatar","urn:image:"+ViewerAvatarFile},
                {"source","iam"},{"status","active"},{"last_ping_at","2026-01-02T10:00:00Z"},
                {"created_at","2026-01-02T10:00:00Z"},{"updated_at","2026-01-02T10:00:00Z"}},
            QJsonObject{{"uuid",channelPeer},{"username","alice"},{"first_name","Alice"},{"last_name","Synthetic"},
                {"email","alice@example.invalid"},{"avatar","urn:image:"+PeerAvatarFile},
                {"source","iam"},{"status","active"},{"last_ping_at","2026-01-02T10:00:00Z"},
                {"created_at","2026-01-02T10:00:00Z"},{"updated_at","2026-01-02T10:00:00Z"}}};
        auto peerMessage=message("The actual peer rail retains its photo while presence changes.",messageId,channelDefault);
        peerMessage["stream_uuid"]=windowDirectId(); peerMessage["author_uuid"]=channelPeer;
        peerMessage["user_uuid"]=userId; peerMessage["is_own"]=false; peerMessage["pinned"]=false;
        peerMessage["reactions"]=QJsonObject{}; peerMessage["reaction_users"]=QJsonObject{};
        peerMessage["updated_at"]=peerMessage.value("created_at");
        const auto original=fixture.server.handler;
        fixture.server.handler=[&,original,peerMessage](const Request &request)->Response {
            const auto path=request.target.path();
            for(auto it=images.cbegin();it!=images.cend();++it) {
                const auto file="/api/workspace/v1/messenger/files/"+it.key();
                if(request.method=="GET"&&path==file) { ++metadataGets[it.key()];return {avatarProbeMetadata(it.key(),it.value())}; }
                if(request.method=="GET"&&path==file+"/actions/download") {
                    ++binaryGets[it.key()];Response response;response.rawBody=it.value();response.rawContentType="image/png";return response;
                }
            }
            auto response=original(request);
            if(request.method=="GET"&&path.endsWith("/messages/")
                &&QUrlQuery(request.target).queryItemValue("stream_uuid")==windowDirectId())
                response.data=QJsonArray{peerMessage};
            return response;
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("presence.ini"));
        ApiClient api(nullptr,true); Session session(&api);
        NativeController controller(&api,&session); FileClient files(&api);controller.useFiles(&files);
        AvatarImages avatars(&api,&session,&files);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences,&avatars);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Presence window did not settle before engine teardown",__FILE__,__LINE__);
        });
        window->resize(1440,900);QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QCOMPARE(window->devicePixelRatio(),qreal(1));
        session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(),2);QTRY_VERIFY(controller.channelMembersVerified());
        preferences.setLocale(locale);preferences.setMode(mode);
        QCOMPARE(preferences.locale(),locale);QCOMPARE(preferences.mode(),mode);
        QTRY_COMPARE(controller.account().value("presence").toString(),QString("active"));
        // Actual Users email supplies the visible profile line; the IAM identity
        // property remains empty. This requires Root's independent displayEmail
        // projection overlay, not an invented Session/IAM fixture value.
        QTRY_COMPARE(controller.account().value("displayEmail").toString(),QString("cassi@example.invalid"));
        QVERIFY(controller.account().value("email").toString().isEmpty());
        QTRY_VERIFY(visibleItem("stream-"+windowDirectId()));
        click(window,visibleItem("stream-"+windowDirectId()));
        QTRY_COMPARE(controller.selectedStream(),windowDirectId());QVERIFY(controller.selectedTopic().isEmpty());
        QTRY_VERIFY(!controller.busy());QTRY_COMPARE(controller.messages().size(),1);
        const auto authorHasPresence=[&](const QString &expected) {
            const auto rows=controller.messages();
            return rows.size()==1&&rows.first().toMap().value("authorPresence").toString()==expected;
        };
        QTRY_COMPARE(controller.directPeer().value("presence").toString(),QString("active"));
        QTRY_VERIFY(authorHasPresence("active"));
        const auto viewer=[&] {return visibleItem("profileAvatar");};
        const auto sidebar=[&] {return avatarProbeCircle(visibleItem("stream-"+windowDirectId()),40);};
        const auto header=[&] {return avatarProbeCircle(visibleItem("directConversationHeader"),40);};
        const auto rail=[&] {return visibleItem("message-avatar-"+messageId);};
        const auto faces=[&] {return QList<QQuickItem *>{viewer(),sidebar(),header(),rail()};};
        QTRY_VERIFY(viewer()&&sidebar()&&header()&&rail());
        // Positive actual photos and their unchanged cover/mask are established
        // BEFORE the first intended old-Main presence assertion.
        QTRY_VERIFY(avatarProbeMatchesPixels(window,viewer(),false));
        QTRY_VERIFY(avatarProbeMatchesPixels(window,sidebar(),true));
        QTRY_VERIFY(avatarProbeMatchesPixels(window,header(),true));
        QTRY_VERIFY(avatarProbeMatchesPixels(window,rail(),true));
        const QList<QSizeF> sizes{{36,36},{40,40},{40,40},{48,48}};
        const auto initialFaces=faces();
        for(qsizetype index=0;index<initialFaces.size();++index) {
            auto *face=initialFaces[index];QVERIFY(face);QCOMPARE(face->size(),sizes[index]);
            auto *image=avatarProbeReadyImage(face);QVERIFY(image);
            QCOMPARE(image->mapRectToItem(face,{QPointF{},image->size()}),QRectF(1,1,face->width()-2,face->height()-2));
            QCOMPARE(QQmlProperty(face,"border.width").read().toReal(),qreal(1));
            QCOMPARE(face->property("radius").toReal(),face->width()/2);
        }
        QTRY_COMPARE(avatars.stats().leases,4);QTRY_COMPARE(avatars.stats().pipelineJobs,0);
        for(const auto &uuid:{ViewerAvatarFile,PeerAvatarFile}) {
            QCOMPARE(metadataGets.value(uuid),1);QCOMPARE(binaryGets.value(uuid),1);
        }
        if(encryptedDirect) {QTRY_VERIFY(visibleItem("encryptedReadStatus"));QVERIFY(!visibleItem("streamTopicPrompt"));}
        else {QTRY_VERIFY(visibleItem("streamTopicPrompt"));QVERIFY(!visibleItem("encryptedReadStatus"));}
        QVERIFY(!controller.canSend());QVERIFY(!controller.canAttach());
        QCOMPARE(visibleItem("conversationHeader")->height(),qreal(56));
        const auto scope=avatars.scopeRevision(),generation=api.generation();
        const auto viewerUrl=avatars.imageUrl("urn:image:"+ViewerAvatarFile);
        const auto peerUrl=avatars.imageUrl("urn:image:"+PeerAvatarFile);
        QVERIFY(!viewerUrl.isEmpty()&&!peerUrl.isEmpty());
        const auto usersBeforeRealtime=fixture.usersGets;
        const QStringList badgeNames{"presence-viewer","presence-stream-"+windowDirectId(),
            "presence-direct-header","presence-author-"+messageId};
        const auto caption=[&](const QString &presence) {
            if(presence=="active") return locale=="ru"?QString("В сети"):QString("online");
            if(presence=="idle") return locale=="ru"?QString("Отошёл"):QString("away");
            return locale=="ru"?QString("Не в сети"):QString("offline");
        };
        const QColor ring(mode=="dark"?"#141517":"#e4ecf3");
        const QStringList statuses{"active","idle","do_not_disturb","offline"};
        for(qsizetype phase=0;phase<statuses.size();++phase) {
            const auto normalized=statuses[phase]=="do_not_disturb"?QString("idle"):statuses[phase];
            const auto ownLabel=phase==1?QString::fromUtf8("💬 <b>Focus</b>"):caption(normalized);
            if(phase>0) {
                for(int who=0;who<2;++who) {
                    auto updated=fixture.users[who].toObject();updated["status"]=statuses[phase];
                    updated["updated_at"]=QDateTime::fromString("2026-01-02T10:00:00Z",Qt::ISODate)
                        .addSecs(phase*10+who).toString(Qt::ISODate);
                    updated["status_text"]=who==0&&phase==1?QString("  <b>Focus</b>  "):QString{};
                    updated["status_emoji"]=who==0&&phase==1?QString("speech_balloon"):QString{};
                    fixture.users[who]=updated;updated["kind"]="user.updated";
                    const int serial=int(phase)*2+who;
                    QVERIFY(controller.applyRealtimeEvent({{"schema_version",1},{"epoch_version",100+serial},
                        {"uuid",QString("a4800000-0000-4000-8000-%1").arg(serial,12,10,QLatin1Char('0'))},
                        {"project_id",projectId},{"user_uuid",userId},{"object_type","user"},{"action","updated"},
                        {"payload",updated},{"created_at",updated.value("updated_at")},{"updated_at",updated.value("updated_at")}}));
                }
            }
            auto *viewport=item(window->contentItem(),"messageList");QVERIFY(viewport);
            if(phase>0) {
                // Actual public user events schedule the existing viewport
                // restoration; one frame is not a readiness contract for it.
                QVERIFY(viewport->property("positioning").toBool());
            }
            QTRY_VERIFY(!viewport->property("positioning").toBool());
            for(const auto &name:badgeNames) QTRY_VERIFY(visibleItem(name));
            QTRY_VERIFY(viewer()&&sidebar()&&header()&&rail());
            QTRY_COMPARE(controller.account().value("presence").toString(),normalized);
            QTRY_COMPARE(controller.directPeer().value("presence").toString(),normalized);
            QTRY_VERIFY(authorHasPresence(normalized));
            // First old-Main RED: no presence-viewer despite real public data and
            // four positively rendered native image surfaces above.
            QTRY_VERIFY(visibleItem(badgeNames.first()));
            QTRY_VERIFY(visibleItem("directPresenceStatus"));
            QTRY_COMPARE(visibleItem("directPresenceStatus")->property("text").toString(),caption(normalized));
            QTRY_VERIFY(visibleItem("profilePresenceStatus"));
            QTRY_COMPARE(visibleItem("profilePresenceStatus")->property("text").toString(),ownLabel);
            QCOMPARE(visibleItem("profilePresenceStatus")->property("textFormat").toInt(),0); // Text.PlainText.
            QCOMPARE(visibleItem("directPresenceStatus")->property("textFormat").toInt(),0);
            QCOMPARE(QQmlProperty(visibleItem("directPresenceStatus"),"font.pixelSize").read().toInt(),12);
            QCOMPARE(visibleItem("directPresenceStatus")->property("lineHeight").toReal(),qreal(16));
            QCOMPARE(visibleItem("directConversationName")->property("lineHeight").toReal(),qreal(20));
            QCOMPARE(QQmlProperty(visibleItem("profilePresenceStatus"),"font.pixelSize").read().toInt(),11);
            QCOMPARE(visibleItem("profilePresenceStatus")->property("lineHeight").toReal(),qreal(13.75));
            QSignalSpy painted(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!painted.isEmpty());
            const QColor fill(normalized=="active"?"#26c038":normalized=="idle"?"#ff8900"
                :mode=="dark"?"#707070":"#989898");
            const auto currentFaces=faces();
            for(qsizetype index=0;index<badgeNames.size();++index) {
                auto *badge=visibleItem(badgeNames[index]);auto *face=currentFaces[index];QVERIFY(badge&&face);
                QCOMPARE(badge->parentItem(),face);QCOMPARE(badge->size(),QSizeF(12,12));
                QCOMPARE(badge->position(),QPointF(face->width()*.8535-6,face->height()*.8535-6));
                QVERIFY(!face->clip());
                auto *dot=item(badge,"presence-fill");QVERIFY(dot);
                QCOMPARE(dot->size(),QSizeF(8,8));QCOMPARE(dot->position(),QPointF(2,2));
                QCOMPARE(dot->property("color").value<QColor>(),fill);
                auto *accessible=QAccessible::queryAccessibleInterface(badge);QVERIFY(accessible);
                QCOMPARE(accessible->role(),QAccessible::StatusBar);QCOMPARE(accessible->text(QAccessible::Name),caption(normalized));
                QVERIFY(!badge->activeFocusOnTab());QVERIFY(!badge->hasActiveFocus());
                QVERIFY(presenceProbeMatchesPixels(window,badge,fill,ring));
            }
            // The caption fits inside the existing topbar; photos keep their
            // dimensions and leases. This does not claim full-shell equivalence.
            auto *profile=visibleItem("accountMenuButton");auto *bar=visibleItem("topBar");QVERIFY(profile&&bar);
            const auto profileRect=profile->mapRectToItem(bar,{QPointF{},profile->size()});
            QVERIFY(QRectF(QPointF{},bar->size()).contains(profileRect));
            const auto profileStatusRect=visibleItem("profilePresenceStatus")->mapRectToItem(profile,{QPointF{},visibleItem("profilePresenceStatus")->size()});
            QVERIFY(QRectF(QPointF{},profile->size()).contains(profileStatusRect));
            QCOMPARE(visibleItem("profileEmail")->property("text").toString(),QString("cassi@example.invalid"));
            QCOMPARE(visibleItem("profileDisplayName")->property("text").toString(),QString("Cassi Synthetic"));
            QCOMPARE(visibleItem("directConversationName")->property("text").toString(),QString("Alice Synthetic"));
            QVERIFY(avatarProbeMatchesPixels(window,viewer(),false));
            QVERIFY(avatarProbeMatchesPixels(window,sidebar(),true));
            QVERIFY(avatarProbeMatchesPixels(window,header(),true));
            QVERIFY(avatarProbeMatchesPixels(window,rail(),true));
            QCOMPARE(avatars.scopeRevision(),scope);QCOMPARE(api.generation(),generation);
            QCOMPARE(avatars.imageUrl("urn:image:"+ViewerAvatarFile),viewerUrl);
            QCOMPARE(avatars.imageUrl("urn:image:"+PeerAvatarFile),peerUrl);
            QTRY_COMPARE(avatars.stats().leases,4);QCOMPARE(avatars.stats().pipelineJobs,0);
            for(const auto &uuid:{ViewerAvatarFile,PeerAvatarFile}) {
                QCOMPARE(metadataGets.value(uuid),1);QCOMPARE(binaryGets.value(uuid),1);
            }
            QCOMPARE(fixture.usersGets,usersBeforeRealtime);QCOMPARE(fixture.posts+fixture.memberPosts,0);
        }
        const auto knownUsers=fixture.users;
        const auto usersBeforeMissing=fixture.usersGets;fixture.users=QJsonArray{};controller.refresh();
        QTRY_VERIFY(fixture.usersGets>usersBeforeMissing);QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(controller.account().value("presence").toString().isEmpty());
        QTRY_VERIFY(controller.directPeer().value("presence").toString().isEmpty());
        QTRY_COMPARE(controller.messages().size(),1);
        QTRY_VERIFY(authorHasPresence({}));
        QTRY_VERIFY(!visibleItem("profilePresenceStatus"));
        for(const auto &name:badgeNames) QTRY_VERIFY(!visibleItem(name));
        QTRY_VERIFY(visibleItem("directPresenceStatus"));
        QCOMPARE(visibleItem("directPresenceStatus")->property("text").toString(),caption("offline"));
        // Restore a positive actual HTTP snapshot before testing owner loss, so
        // hidden badges after sign-out cannot pass vacuously from missing users.
        const auto usersBeforeRestore=fixture.usersGets;fixture.users=knownUsers;controller.refresh();
        QTRY_VERIFY(fixture.usersGets>usersBeforeRestore);QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.directPeer().value("presence").toString(),QString("offline"));
        for(const auto &name:badgeNames) QTRY_VERIFY(visibleItem(name));
        QTRY_COMPARE(avatars.stats().leases,4);QTRY_COMPARE(avatars.stats().pipelineJobs,0);
        for(const auto &uuid:{ViewerAvatarFile,PeerAvatarFile}) {
            QCOMPARE(metadataGets.value(uuid),1);QCOMPARE(binaryGets.value(uuid),1);
        }
        click(window,visibleItem("accountMenuButton"));QTRY_VERIFY(visibleItem("signOutButton"));
        QSignalSpy menuFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!menuFrame.isEmpty());
        auto *signOut=visibleItem("signOutButton");QVERIFY(signOut);
        QVERIFY(QRectF(QPointF{},window->size()).contains(signOut->mapRectToScene({QPointF{},signOut->size()})));
        QSignalSpy signedOutClick(signOut,SIGNAL(clicked()));QVERIFY(signedOutClick.isValid());click(window,signOut);
        QTRY_COMPARE(signedOutClick.size(),1);QTRY_VERIFY(!controller.authenticated());
        for(const auto &name:badgeNames) QTRY_VERIFY(!visibleItem(name));
        QVERIFY(!visibleItem("profilePresenceStatus"));QVERIFY(!visibleItem("directPresenceStatus"));
        QTRY_COMPARE(avatars.stats().leases,0);QCOMPARE(avatars.stats().pipelineJobs,0);
        QVERIFY(controller.account().value("presence").toString().isEmpty());
        QCOMPARE(fixture.posts+fixture.memberPosts,0);
        for(const auto &uuid:{ViewerAvatarFile,PeerAvatarFile}) {
            QCOMPARE(metadataGets.value(uuid),1);QCOMPARE(binaryGets.value(uuid),1);
        }
        const bool closed=closeIncubatedTestWindow(engine,window);
        if(closed) closeBeforeEngine.dismiss();QVERIFY(closed);
    }

    void avatarHttpFailureKeepsFallbackAndRealtimeReplacementUpdatesPeerFaces()
    {
        const QString recoveredFile="a4300000-0000-4000-8000-000000000003";
        const QString viewerUrn="urn:image:"+ViewerAvatarFile;
        const QString failedUrn="urn:image:"+PeerAvatarFile;
        const QString recoveredUrn="urn:image:"+recoveredFile;
        QMap<QString,int> metadataGets,binaryGets;
        std::function<void()> deliverOldFailure;
        const auto viewerPng=avatarProbePng(false), recoveredPng=avatarProbePng(true);
        QCOMPARE(QCryptographicHash::hash(viewerPng,QCryptographicHash::Sha256).toHex(),
                 QByteArray("9fb39a2b2737f88bdf0b383c4fcea4b0f3636e584b280f10afd18444739ed1f9"));
        QCOMPARE(QCryptographicHash::hash(recoveredPng,QCryptographicHash::Sha256).toHex(),
                 QByteArray("d6a802c7cdef5460f2665fb94eb5d46e4b65e03186217cdaa868b9c128541de6"));
        const QMap<QString,QByteArray> images{{ViewerAvatarFile,viewerPng},
            {PeerAvatarFile,viewerPng},{recoveredFile,recoveredPng}};
        WindowDirectFixture fixture; fixture.seed();
        fixture.created["encryption"]=true;
        fixture.created["current_encryption_key_uuid"]=QJsonValue::Null;
        fixture.users=QJsonArray{
            QJsonObject{{"uuid",userId},{"username","cassi"},{"first_name","Cassi"},{"last_name","Synthetic"},
                {"email","cassi@example.invalid"},{"avatar",viewerUrn},{"source","iam"},{"status","offline"},
                {"last_ping_at","2026-01-02T10:00:00Z"},{"created_at","2026-01-02T10:00:00Z"},
                {"updated_at","2026-01-02T10:00:00Z"}},
            QJsonObject{{"uuid",channelPeer},{"username","alice"},{"first_name","Alice"},{"last_name","Synthetic"},
                {"email","alice@example.invalid"},{"avatar",failedUrn},{"source","iam"},{"status","offline"},
                {"last_ping_at","2026-01-02T10:00:00Z"},{"created_at","2026-01-02T10:00:00Z"},
                {"updated_at","2026-01-02T10:00:00Z"}}};
        auto peerMessage=message("A real peer history row keeps the avatar rail mounted through its image failure and replacement.",
                                 messageId,channelDefault);
        peerMessage["stream_uuid"]=windowDirectId(); peerMessage["author_uuid"]=channelPeer;
        peerMessage["user_uuid"]=userId; peerMessage["is_own"]=false;
        peerMessage["pinned"]=false; peerMessage["reactions"]=QJsonObject{};
        peerMessage["reaction_users"]=QJsonObject{}; peerMessage["updated_at"]=peerMessage.value("created_at");
        const auto original=fixture.server.handler;
        fixture.server.handler=[&,original,peerMessage](const Request &request)->Response {
            const auto path=request.target.path();
            for(auto i=images.cbegin();i!=images.cend();++i) {
                const auto file="/api/workspace/v1/messenger/files/"+i.key();
                if(request.method=="GET"&&path==file) {
                    ++metadataGets[i.key()]; return {avatarProbeMetadata(i.key(),i.value())};
                }
                if(request.method=="GET"&&path==file+"/actions/download") {
                    ++binaryGets[i.key()]; Response response;
                    if(i.key()==PeerAvatarFile) {
                        response.status=403; response.rawBody="synthetic avatar access denied";
                        response.rawContentType="text/plain";
                        response.defer=[&](std::function<void()> send) { deliverOldFailure=std::move(send); };
                    } else { response.rawBody=i.value(); response.rawContentType="image/png"; }
                    return response;
                }
            }
            auto response=original(request);
            if(request.method=="GET"&&path.endsWith("/messages/")
                &&QUrlQuery(request.target).queryItemValue("stream_uuid")==windowDirectId())
                response.data=QJsonArray{peerMessage};
            return response;
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("avatar-recovery.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        ApiClient api(nullptr,true); Session session(&api);
        NativeController controller(&api,&session); FileClient files(&api); controller.useFiles(&files);
        AvatarImages avatars(&api,&session,&files);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences,&avatars);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Avatar recovery window did not settle before teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->resize(1440,900);
        QCOMPARE(window->devicePixelRatio(),qreal(1));
        session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(),2); QTRY_VERIFY(controller.channelMembersVerified());
        QTRY_COMPARE(controller.viewerDisplayName(),QString("Cassi Synthetic"));
        QTRY_VERIFY(visibleItem("stream-"+windowDirectId()));
        click(window,visibleItem("stream-"+windowDirectId()));
        QTRY_COMPARE(controller.selectedStream(),windowDirectId()); QVERIFY(controller.selectedTopic().isEmpty());
        QTRY_VERIFY(!controller.busy()); QTRY_VERIFY(visibleItem("message-avatar-"+messageId));
        QTRY_COMPARE(controller.directPeer().value("displayName").toString(),QString("Alice Synthetic"));
        QTRY_VERIFY(visibleItem("encryptedReadStatus")); QVERIFY(!visibleItem("streamTopicPrompt"));
        QVERIFY(!controller.encryption().value("available").toBool());
        const auto viewer=[&] { return visibleItem("profileAvatar"); };
        const auto sidebar=[&] { return avatarProbeCircle(visibleItem("stream-"+windowDirectId()),40); };
        const auto header=[&] { return avatarProbeCircle(visibleItem("directConversationHeader"),40); };
        const auto rail=[&] { return visibleItem("message-avatar-"+messageId); };
        QTRY_VERIFY(viewer()&&sidebar()&&header()&&rail());
        QTRY_VERIFY(avatarProbeMatchesPixels(window,viewer(),false));
        QTRY_COMPARE(binaryGets.value(PeerAvatarFile),1); // Actual binary GET acknowledged before any release/mutation.
        QTRY_VERIFY(bool(deliverOldFailure));
        const auto activeOldBinary=[&] {
            for(auto *reply:api.findChildren<QNetworkReply *>())
                if(!reply->isFinished()&&reply->url().path()==
                    "/api/workspace/v1/messenger/files/"+PeerAvatarFile+"/actions/download") return true;
            return false;
        };
        QTRY_VERIFY(activeOldBinary());
        QTRY_COMPARE(avatars.stats().leases,4); // Viewer + all3 actual peer surfaces are mounted BEFORE failure.
        QCOMPARE(avatars.stats().pipelineJobs,1); QVERIFY(!avatars.failed(failedUrn));
        QCOMPARE(metadataGets.value(PeerAvatarFile),1);
        const auto scope=avatars.scopeRevision(), generation=api.generation();
        const auto viewerUrl=avatars.imageUrl(viewerUrn); QVERIFY(!viewerUrl.isEmpty());
        auto releaseFailure=std::move(deliverOldFailure); deliverOldFailure={}; releaseFailure();
        QTRY_VERIFY(avatars.failed(failedUrn)); QTRY_COMPARE(avatars.stats().pipelineJobs,0);
        const auto fallbackVisible=[&] {
            auto *s=sidebar(),*h=header(),*r=rail();
            return s&&h&&r&&avatarProbeHasVisibleLock(s)&&hasVisibleText(h,"A")&&hasVisibleText(r,"A")
                &&!avatarProbeReadyImage(s)&&!avatarProbeReadyImage(h)&&!avatarProbeReadyImage(r);
        };
        QTRY_VERIFY2(fallbackVisible(),"HTTP image failure must retain the existing Direct lock and peer initials");
        QSignalSpy failedFrame(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!failedFrame.isEmpty());
        QCOMPARE(avatars.stats().leases,4); QCOMPARE(avatars.stats().thumbnails,1);
        QVERIFY(avatarProbeMatchesPixels(window,viewer(),false));
        QCOMPARE(metadataGets.value(PeerAvatarFile),1); QCOMPARE(binaryGets.value(PeerAvatarFile),1);
        QCOMPARE(metadataGets.value(ViewerAvatarFile),1); QCOMPARE(binaryGets.value(ViewerAvatarFile),1);
        const auto usersBefore=fixture.usersGets;
        const auto context=controller.directChatContextKey();
        auto updated=fixture.users.at(1).toObject(); updated["avatar"]=recoveredUrn;
        updated["updated_at"]="2026-01-02T10:01:00Z"; fixture.users[1]=updated;
        updated["kind"]="user.updated";
        QSignalSpy streamNotice(&controller,&NativeController::streamsChanged),
            messageNotice(&controller,&NativeController::messagesChanged);
        QVERIFY(streamNotice.isValid()&&messageNotice.isValid());
        QVERIFY(controller.applyRealtimeEvent({{"schema_version",1},{"epoch_version",81},
            {"uuid","a4300000-0000-4000-8000-000000000099"},{"project_id",projectId},{"user_uuid",userId},
            {"object_type","user"},{"action","updated"},{"payload",updated},
            {"created_at","2026-01-02T10:01:00Z"},{"updated_at","2026-01-02T10:01:00Z"}}));
        QVERIFY(!streamNotice.isEmpty()&&!messageNotice.isEmpty());
        QTRY_COMPARE(controller.directPeer().value("avatar").toString(),recoveredUrn);
        QCOMPARE(controller.account().value("avatar").toString(),viewerUrn);
        QCOMPARE(controller.directChatContextKey(),context); QCOMPARE(api.generation(),generation);
        QCOMPARE(avatars.scopeRevision(),scope);
        // Always relookup Main faces: real user.updated can rebuild model delegates.
        QTRY_VERIFY2(avatarProbeMatchesPixels(window,sidebar(),true),"Realtime avatar URN replacement must update the Direct sidebar");
        QTRY_VERIFY2(avatarProbeMatchesPixels(window,header(),true),"Realtime avatar URN replacement must update the Direct header");
        QTRY_VERIFY2(avatarProbeMatchesPixels(window,rail(),true),"Realtime avatar URN replacement must update the actual peer rail");
        QTRY_VERIFY(avatarProbeMatchesPixels(window,viewer(),false));
        QCOMPARE(avatars.imageUrl(viewerUrn),viewerUrl); QTRY_COMPARE(avatars.stats().pipelineJobs,0);
        QTRY_COMPARE(avatars.stats().leases,4); QCOMPARE(avatars.stats().thumbnails,2);
        for(auto *face:{viewer(),sidebar(),header(),rail()}) {
            QVERIFY(face); auto *image=avatarProbeReadyImage(face); QVERIFY(image);
            QCOMPARE(image->mapRectToItem(face,QRectF(QPointF{},image->size())),QRectF(1,1,face->width()-2,face->height()-2));
        }
        QVERIFY(!avatarProbeHasVisibleLock(sidebar())); QVERIFY(!hasVisibleText(header(),"A")); QVERIFY(!hasVisibleText(rail(),"A"));
        QCOMPARE(metadataGets.value(recoveredFile),1); QCOMPARE(binaryGets.value(recoveredFile),1);
        QCOMPARE(metadataGets.value(PeerAvatarFile),1); QCOMPARE(binaryGets.value(PeerAvatarFile),1);
        QCOMPARE(metadataGets.value(ViewerAvatarFile),1); QCOMPARE(binaryGets.value(ViewerAvatarFile),1);
        QCOMPARE(fixture.usersGets,usersBefore); QCOMPARE(fixture.posts+fixture.memberPosts,0);
        QCOMPARE(controller.selectedStream(),windowDirectId()); QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(controller.directPeer().value("displayName").toString(),QString("Alice Synthetic"));
        QCOMPARE(visibleItem("conversationHeader")->height(),qreal(56));
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        for(const auto &request:fixture.server.requests) {
            if(!request.target.path().startsWith("/api/workspace/v1/messenger/files/")) continue;
            QCOMPARE(request.method,QByteArray("GET"));
            QCOMPARE(request.headers.value("authorization"),QByteArray("Bearer synthetic-project-token"));
            QVERIFY(request.headers.value("cookie").isEmpty());
        }
        const bool closed=closeIncubatedTestWindow(engine,window);
        if(closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
        QTRY_COMPARE(avatars.stats().leases,0); QCOMPARE(avatars.stats().pipelineJobs,0);
        QVERIFY(avatars.stats().entries<=AvatarImages::MaximumEntries);
    }

    void avatarComponentKeepsNestedScopeAndReferenceLease()
    {
        // Additive component contract test. Requires the accepted real service,
        // Root's real native-app binding and avatarProbe* helpers from the pixel
        // proposal. Both compared closures use exactly this fixture and observer;
        // only NativeAvatarImage.qml differs (Root v1 vs atomic v2).
        QMap<QString,int> metadataGets, binaryGets;
        const auto viewerPng = avatarProbePng(false), peerPng = avatarProbePng(true);
        const QMap<QString,QByteArray> images{{ViewerAvatarFile,viewerPng},{PeerAvatarFile,peerPng}};
        WindowDirectFixture fixture; fixture.seed();
        const auto original = fixture.server.handler;
        fixture.server.handler = [&,original](const Request &request)->Response {
            const auto path = request.target.path();
            for (auto it=images.cbegin(); it!=images.cend(); ++it) {
                const auto file = "/api/workspace/v1/messenger/files/"+it.key();
                if (request.method=="GET" && path==file) {
                    ++metadataGets[it.key()]; return {avatarProbeMetadata(it.key(),it.value())};
                }
                if (request.method=="GET" && path==file+"/actions/download") {
                    ++binaryGets[it.key()]; Response response;
                    response.rawBody=it.value(); response.rawContentType="image/png"; return response;
                }
            }
            return original(request);
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("avatar-reentry.ini"));
        ApiClient api(nullptr,true); Session session(&api);
        NativeController controller(&api,&session); FileClient files(&api);
        controller.useFiles(&files);
        AvatarImages avatars(&api,&session,&files);
        QPointer<QQuickItem> photo;
        bool armed=false, reentered=false, changedReference=false, advancedGeneration=false;
        bool readyAtBoundary=false, nestedVisible=false;
        int nestedLeaseCount=-1;
        quint64 initialGeneration=0, initialScope=0;
        TopicCompletionProbe probe; // Existing safe QML -> C++ notifier relay.
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("nativeAvatarReentryProbe",&probe);
        loadNativeUi(engine,&controller,&preferences,&avatars);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Avatar reentry window did not settle before teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1); session.selectProject(projectId);
        QTRY_VERIFY(session.ready()); QTRY_VERIFY(controller.authenticated());
        QCOMPARE(avatars.stats().leases,0); // Fixture accounts have no avatar URNs.
        QCOMPARE(metadataGets.size(),0); QCOMPARE(binaryGets.size(),0);
        const auto connection=connect(&probe,&TopicCompletionProbe::completed,&avatars,
            [&](const QString &,const QString &url) {
                if(!armed || reentered || !url.isEmpty() || !photo) return;
                // Guard recursion BEFORE actual outward calls. No QVERIFY/QTRY or
                // event pumping in this synchronous notifier callback.
                reentered=true; armed=false;
                readyAtBoundary=session.ready();
                api.setAccessToken("synthetic-avatar-reentrant-generation");
                advancedGeneration=api.generation()>initialGeneration;
                if(!photo) return;
                changedReference=photo->setProperty("reference","urn:image:"+PeerAvatarFile);
                if(!photo) return;
                photo->setVisible(true);
                nestedVisible=photo && photo->isVisible(); nestedLeaseCount=avatars.stats().leases;
            });
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import "qrc:/workspace/qml"
            NativeAvatarImage {
                id: subject
                objectName: "avatarReentrySubject"
                width: 36; height: 36
                images: nativeAvatars
                Connections {
                    target: subject
                    function onImageUrlChanged() {
                        nativeAvatarReentryProbe.completed(subject.reference,subject.imageUrl)
                    }
                }
            }
        )QML",QUrl("qrc:/native-avatar-reentry-test.qml"));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        std::unique_ptr<QQuickItem> subject(qobject_cast<QQuickItem *>(component.create()));
        const auto disconnectObserver=qScopeGuard([&armed,connection] { armed=false; QObject::disconnect(connection); });
        QVERIFY2(subject,qPrintable(component.errorString()));
        photo=subject.get(); photo->setParentItem(window->contentItem());
        photo->setPosition(QPointF(90,90));
        QVERIFY(photo->setProperty("reference","urn:image:"+ViewerAvatarFile));
        QTRY_VERIFY(!avatars.imageUrl("urn:image:"+ViewerAvatarFile).isEmpty());
        QTRY_VERIFY(avatarProbeReadyImage(photo.data()));
        QCOMPARE(avatars.stats().leases,1);
        QCOMPARE(metadataGets.value(ViewerAvatarFile),1); QCOMPARE(binaryGets.value(ViewerAvatarFile),1);
        const auto oldUrl=avatars.imageUrl("urn:image:"+ViewerAvatarFile);
        std::unique_ptr<QQuickImageProvider> provider(avatars.createImageProvider());
        initialGeneration=api.generation(); initialScope=avatars.scopeRevision();
        armed=true;
        photo->setVisible(false); // Actual public visibility notification starts refresh.
        QVERIFY(reentered); QVERIFY(readyAtBoundary); QVERIFY(advancedGeneration);
        QVERIFY(changedReference); QVERIFY(nestedVisible); QCOMPARE(nestedLeaseCount,1);
        QVERIFY(avatars.scopeRevision()>initialScope);
        QVERIFY(photo && photo->isVisible());
        QCOMPARE(photo->property("reference").toString(),"urn:image:"+PeerAvatarFile);
        QTRY_VERIFY(!avatars.imageUrl("urn:image:"+PeerAvatarFile).isEmpty());
        QCOMPARE(metadataGets.value(PeerAvatarFile),1); QCOMPARE(binaryGets.value(PeerAvatarFile),1);
        // First intended v1 failure: the real manager has downloaded+decoded B,
        // but outer dual-property clear orphaned the nested component ownership.
        QTRY_VERIFY2(avatarProbeReadyImage(photo.data()),"Nested actual B acquisition must remain visible after outer refresh returns");
        QCOMPARE(photo->property("imageUrl").toString(),avatars.imageUrl("urn:image:"+PeerAvatarFile));
        QCOMPARE(avatars.stats().leases,1);
        QSize size;
        const auto retired=provider->requestImage(QUrl(oldUrl).path().mid(1),&size,QSize{});
        QCOMPARE(retired.size(),QSize(1,1)); QCOMPARE(retired.pixelColor(0,0).alpha(),0);
        photo->setVisible(false);
        QCOMPARE(avatars.stats().leases,0);
        const auto finalUrl=avatars.imageUrl("urn:image:"+PeerAvatarFile);
        const auto finalScope=avatars.scopeRevision();
        api.setAccessToken("synthetic-avatar-hidden-generation");
        QVERIFY(avatars.scopeRevision()>finalScope);
        QCOMPARE(avatars.stats().leases,0); QCOMPARE(avatars.stats().pipelineJobs,0);
        QVERIFY(avatars.imageUrl("urn:image:"+PeerAvatarFile).isEmpty());
        QVERIFY(photo->property("imageUrl").toString().isEmpty());
        const auto hiddenRetired=provider->requestImage(QUrl(finalUrl).path().mid(1),&size,QSize{});
        QCOMPARE(hiddenRetired.size(),QSize(1,1)); QCOMPARE(hiddenRetired.pixelColor(0,0).alpha(),0);
        QCOMPARE(metadataGets.value(ViewerAvatarFile),1); QCOMPARE(binaryGets.value(ViewerAvatarFile),1);
        QCOMPARE(metadataGets.value(PeerAvatarFile),1); QCOMPARE(binaryGets.value(PeerAvatarFile),1);
    }

    void messageImageComponentKeepsNestedScopeAndReferenceLease_data()
    {
        QTest::addColumn<bool>("atRefreshTicket");
        QTest::newRow("image-url-scope-reference") << false;
        QTest::newRow("refresh-ticket-scope-reference") << true;
    }

    void messageImageComponentKeepsNestedScopeAndReferenceLease()
    {
        QFETCH(bool,atRefreshTicket);
        QTest::failOnWarning(QRegularExpression(".*"));
        QMap<QString,int> metadataGets,binaryGets;
        const QMap<QString,QByteArray> bodies{{ViewerAvatarFile,avatarProbePng(false)},
                                             {PeerAvatarFile,avatarProbePng(true)}};
        const auto urnA="urn:image:"+ViewerAvatarFile,urnB="urn:image:"+PeerAvatarFile;
        WindowDirectFixture fixture;fixture.seed();
        const auto original=fixture.server.handler;
        fixture.server.handler=[&,original](const Request &request)->Response {
            const auto path=request.target.path();
            for(auto i=bodies.cbegin();i!=bodies.cend();++i) {
                const auto file="/api/workspace/v1/messenger/files/"+i.key();
                if(request.method=="GET"&&path==file) {
                    ++metadataGets[i.key()];return {avatarProbeMetadata(i.key(),i.value())};
                }
                if(request.method=="GET"&&path==file+"/actions/download") {
                    ++binaryGets[i.key()];Response result;
                    result.rawBody=i.value();result.rawContentType="image/png";return result;
                }
            }
            return original(request);
        };
        QTemporaryDir directory;QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("message-image-reentry.ini"));
        ApiClient api(nullptr,true);Session session(&api);
        NativeController controller(&api,&session);FileClient files(&api);controller.useFiles(&files);
        MessageImages images(&api,&session,&files);
        std::unique_ptr<QQuickImageProvider> provider(images.createImageProvider());
        QPointer<QQuickItem> photo;
        bool armed=false,reentered=false,readyAtBoundary=false,advancedGeneration=false;
        bool changedReference=false,nestedVisible=false;
        int nestedLeaseCount=-1;
        quint64 initialGeneration=0,initialScope=0;
        TopicCompletionProbe probe;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("nativeMessageReentryProbe",&probe);
        loadNativeUi(engine,&controller,&preferences,nullptr,nullptr,&images);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Message image reentry window did not settle before teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        engine.rootContext()->setContextProperty("nativeMessageReentryWindow",window);
        session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);
        QTRY_VERIFY(session.ready());QTRY_VERIFY(controller.authenticated());
        QCOMPARE(images.stats().leases,0);QCOMPARE(images.stats().pipelineJobs,0);
        QVERIFY(metadataGets.isEmpty()&&binaryGets.isEmpty()); // No prewarm.
        std::unique_ptr<QQuickItem> holder;
        const auto connection=connect(&probe,&TopicCompletionProbe::completed,&images,
            [&](const QString &kind,const QString &value) {
                const auto expected=atRefreshTicket?QString("ticket"):QString("url");
                if(!armed||reentered||!photo||kind!=expected||(!atRefreshTicket&&!value.isEmpty()))return;
                reentered=true;armed=false; // Before outward calls; no pumping/assertions here.
                readyAtBoundary=session.ready();
                api.setAccessToken("synthetic-message-image-reentrant-generation");
                advancedGeneration=api.generation()>initialGeneration;
                if(!photo)return;
                changedReference=photo->setProperty("reference",urnB);
                if(!photo)return;
                photo->setVisible(true);
                nestedVisible=photo&&photo->isVisible();nestedLeaseCount=images.stats().leases;
            });
        const auto disconnectObserver=qScopeGuard([&armed,connection] {armed=false;QObject::disconnect(connection);});
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import "qrc:/workspace/qml"
            Flickable {
                id: band
                width: 320; height: 240; contentWidth: width; contentHeight: 480
                clip: true; z: 100
                NativeMessageImage {
                    id: subject
                    objectName: "messageImageReentrySubject"
                    width: 96; x: 0; y: 40
                    images: nativeMessageImages
                    colors: nativeMessageReentryWindow.colors
                    viewport: band
                    Connections {
                        target: subject
                        function onImageUrlChanged() {
                            nativeMessageReentryProbe.completed("url",subject.imageUrl)
                        }
                        function onRefreshTicketChanged() {
                            nativeMessageReentryProbe.completed("ticket",String(subject.refreshTicket))
                        }
                    }
                }
            }
        )QML",QUrl("qrc:/native-message-image-reentry-test.qml"));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        holder.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(holder,qPrintable(component.errorString()));
        holder->setParentItem(window->contentItem());holder->setPosition(QPointF(90,90));
        photo=item(holder.get(),"messageImageReentrySubject");QVERIFY(photo);
        std::function<QQuickItem *(QQuickItem *)> readyImage=[&](QQuickItem *root)->QQuickItem * {
            if(!root||!root->isVisible())return nullptr;
            const auto source=root->property("source").toUrl();
            if(source.scheme()=="image"&&source.host()=="messageimages"&&root->property("status").toInt()==1)return root;
            for(auto *child:root->childItems())if(auto *found=readyImage(child))return found;
            return nullptr;
        };
        QVERIFY(photo->setProperty("reference",urnA));
        QTRY_VERIFY(!images.imageUrl(urnA).isEmpty());QTRY_VERIFY(readyImage(photo.data()));
        QCOMPARE(photo->size(),QSizeF(96,64));QVERIFY(photo->property("loadActive").toBool());
        QCOMPARE(images.stats().leases,1);QTRY_COMPARE(images.stats().pipelineJobs,0);
        QCOMPARE(metadataGets.value(ViewerAvatarFile),1);QCOMPARE(binaryGets.value(ViewerAvatarFile),1);
        const auto oldUrl=images.imageUrl(urnA);
        initialGeneration=api.generation();initialScope=images.scopeRevision();
        armed=true;
        photo->setVisible(false); // Actual visibility -> demand release; no readiness injection.
        QVERIFY(reentered);QVERIFY(readyAtBoundary);QVERIFY(advancedGeneration);
        QVERIFY(changedReference);QVERIFY(nestedVisible);QCOMPARE(nestedLeaseCount,1);
        QVERIFY(images.scopeRevision()>initialScope);QVERIFY(photo&&photo->isVisible());
        QCOMPARE(photo->property("reference").toString(),urnB);
        // Intended ticket-row consequence: stale outer refresh must not release the nested lease.
        QCOMPARE(images.stats().leases,1);
        QTRY_VERIFY(!images.imageUrl(urnB).isEmpty());QTRY_VERIFY(readyImage(photo.data()));
        QCOMPARE(photo->property("imageUrl").toString(),images.imageUrl(urnB));
        QCOMPARE(photo->size(),QSizeF(96,64));QCOMPARE(images.stats().leases,1);
        QTRY_COMPARE(images.stats().pipelineJobs,0);QCOMPARE(images.stats().entries,1);
        QCOMPARE(images.stats().previews,1);QCOMPARE(images.stats().previewBytes,qint64(96*64*4));
        QCOMPARE(metadataGets.value(PeerAvatarFile),1);QCOMPARE(binaryGets.value(PeerAvatarFile),1);
        const auto expected=QImage::fromData(bodies.value(PeerAvatarFile));QCOMPARE(expected.size(),QSize(96,64));
        QSize providerSize;
        const auto actual=provider->requestImage(QUrl(images.imageUrl(urnB)).path().mid(1),&providerSize,QSize{});
        QCOMPARE(providerSize,expected.size());QCOMPARE(actual.size(),expected.size());
        for(const QPoint &point:{QPoint(24,16),QPoint(72,16),QPoint(24,48),QPoint(72,48)})
            QCOMPARE(actual.pixelColor(point),expected.pixelColor(point));
        const auto stale=provider->requestImage(QUrl(oldUrl).path().mid(1),&providerSize,QSize{});
        QCOMPARE(stale.size(),QSize(1,1));QCOMPARE(stale.pixelColor(0,0).alpha(),0);
        QVERIFY(images.imageUrl(urnA).isEmpty());
        QSignalSpy frame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!frame.isEmpty());
        const auto realPixels=[&] {
            if(!photo||!readyImage(photo.data()))return false;
            const auto capture=window->grabWindow();if(capture.isNull())return false;
            const auto scene=photo->mapToScene(QPointF(24,16));
            const QPoint sample(qRound(scene.x()*window->devicePixelRatio()),qRound(scene.y()*window->devicePixelRatio()));
            if(!capture.rect().contains(sample))return false;
            const auto color=capture.pixelColor(sample),wanted=expected.pixelColor(24,16);
            return qAbs(color.red()-wanted.red())<=3&&qAbs(color.green()-wanted.green())<=3
                &&qAbs(color.blue()-wanted.blue())<=3&&color.alpha()==255;
        };
        QTRY_VERIFY2(realPixels(),"Nested B must paint actual verified provider pixels in the visible window");
        const auto finalUrl=images.imageUrl(urnB);const auto finalScope=images.scopeRevision();
        photo->setVisible(false);
        QCOMPARE(images.stats().leases,0);QCOMPARE(images.stats().pipelineJobs,0);
        QCOMPARE(photo->size(),QSizeF(96,64)); // Ordinary release preserves measured layout.
        QVERIFY(photo->property("imageUrl").toString().isEmpty());
        api.setAccessToken("synthetic-message-image-hidden-generation");
        QVERIFY(images.scopeRevision()>finalScope);
        QCOMPARE(images.stats().leases,0);QCOMPARE(images.stats().pipelineJobs,0);
        QCOMPARE(images.stats().entries,0);QCOMPARE(images.stats().previewBytes,qint64(0));
        QVERIFY(images.imageUrl(urnB).isEmpty());QVERIFY(photo->property("imageUrl").toString().isEmpty());
        const auto hidden=provider->requestImage(QUrl(finalUrl).path().mid(1),&providerSize,QSize{});
        QCOMPARE(hidden.size(),QSize(1,1));QCOMPARE(hidden.pixelColor(0,0).alpha(),0);
        for(const auto &request:fixture.server.requests) {
            if(!request.target.path().startsWith("/api/workspace/v1/messenger/files/"))continue;
            QCOMPARE(request.method,QByteArray("GET"));QVERIFY(request.headers.value("cookie").isEmpty());
        }
        QCOMPARE(fixture.posts+fixture.memberPosts,0);
        holder.reset();
        const bool closed=closeIncubatedTestWindow(engine,window);
        if(closed)closeBeforeEngine.dismiss();QVERIFY(closed);
        QCOMPARE(images.stats().leases,0);QCOMPARE(images.stats().pipelineJobs,0);
    }


    void uploadedAvatarsUseRealMainPixels_data()
    {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("encryptedDirect");
        QTest::newRow("en-light-direct") << QString("en") << QString("light") << false;
        QTest::newRow("ru-dark-direct") << QString("ru") << QString("dark") << false;
        QTest::newRow("en-light-protected-direct") << QString("en") << QString("light") << true;
        QTest::newRow("ru-dark-protected-direct") << QString("ru") << QString("dark") << true;
    }

    void uploadedAvatarsUseRealMainPixels()
    {
        QFETCH(QString, locale); QFETCH(QString, mode); QFETCH(bool, encryptedDirect);
        QMap<QString,int> metadataGets, binaryGets;
        const auto captures=qEnvironmentVariable("CASSI_NATIVE_AVATAR_SCREENSHOT_DIRECTORY");
        QJsonArray servedResponses; // Only synthetic public Workspace collection payloads.
        const auto viewerPng=avatarProbePng(false), peerPng=avatarProbePng(true);
        QCOMPARE(QCryptographicHash::hash(viewerPng,QCryptographicHash::Sha256).toHex(),
                 QByteArray("9fb39a2b2737f88bdf0b383c4fcea4b0f3636e584b280f10afd18444739ed1f9"));
        QCOMPARE(QCryptographicHash::hash(peerPng,QCryptographicHash::Sha256).toHex(),
                 QByteArray("d6a802c7cdef5460f2665fb94eb5d46e4b65e03186217cdaa868b9c128541de6"));
        const QMap<QString,QByteArray> images{{ViewerAvatarFile,viewerPng},{PeerAvatarFile,peerPng}};
        WindowDirectFixture fixture; fixture.seed();
        fixture.created["encryption"]=encryptedDirect;
        // Existing encrypted DM metadata is readable without bootstrap/key material.
        // This fixture does not claim encrypted message/decrypt/composer acceptance.
        fixture.created["current_encryption_key_uuid"]=QJsonValue::Null;
        fixture.users=QJsonArray{
            QJsonObject{{"uuid",userId},{"username","cassi"},{"first_name","Cassi"},{"last_name","Synthetic"},
                {"email","cassi@example.invalid"},{"avatar","urn:image:"+ViewerAvatarFile},
                {"source","iam"},{"status","offline"},{"last_ping_at","2026-01-02T10:00:00Z"},
                {"created_at","2026-01-02T10:00:00Z"},{"updated_at","2026-01-02T10:00:00Z"}},
            QJsonObject{{"uuid",channelPeer},{"username","alice"},{"first_name","Alice"},{"last_name","Synthetic"},
                {"email","alice@example.invalid"},{"avatar","urn:image:"+PeerAvatarFile},
                {"source","iam"},{"status","offline"},{"last_ping_at","2026-01-02T10:00:00Z"},
                {"created_at","2026-01-02T10:00:00Z"},{"updated_at","2026-01-02T10:00:00Z"}}};
        auto peerMessage=message("A public synthetic peer message keeps the actual avatar rail visible. "
                                 "The picture belongs to Alice Synthetic, not the current viewer.",messageId,channelDefault);
        peerMessage["stream_uuid"]=windowDirectId(); peerMessage["author_uuid"]=channelPeer;
        peerMessage["user_uuid"]=userId; peerMessage["is_own"]=false;
        peerMessage["pinned"]=false; peerMessage["reactions"]=QJsonObject{};
        peerMessage["reaction_users"]=QJsonObject{};
        peerMessage["updated_at"]=peerMessage.value("created_at");
        const auto original=fixture.server.handler;
        fixture.server.handler=[&,original,peerMessage](const Request &request)->Response {
            const auto path=request.target.path();
            for(auto it=images.cbegin();it!=images.cend();++it) {
                const auto file="/api/workspace/v1/messenger/files/"+it.key();
                if(request.method=="GET"&&path==file) { ++metadataGets[it.key()]; return {avatarProbeMetadata(it.key(),it.value())}; }
                if(request.method=="GET"&&path==file+"/actions/download") {
                    ++binaryGets[it.key()]; Response response; response.rawBody=it.value();
                    response.rawContentType="image/png"; return response;
                }
            }
            auto response=original(request);
            if(request.method=="GET"&&path.endsWith("/messages/")
                &&QUrlQuery(request.target).queryItemValue("stream_uuid")==windowDirectId())
                response.data=QJsonArray{peerMessage};
            if (!captures.isEmpty() && request.method=="GET" && path.startsWith("/api/workspace/")
                && (path.endsWith("/users/") || path.endsWith("/streams/")
                    || path.endsWith("/stream_topics/") || path.endsWith("/messages/")))
                servedResponses.append(QJsonObject{{"target",request.target.toString(QUrl::FullyEncoded)},
                    {"status",response.status},{"body",response.data}});
            return response;
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("appearance.ini"));
        preferences.setLocale(locale); preferences.setMode(mode);
        ApiClient api(nullptr,true); Session session(&api);
        NativeController controller(&api,&session); FileClient files(&api); controller.useFiles(&files);
        // Controlled Main-only comparison: same REAL service setup in both closures.
        // Requires Root's separately reviewed optional loadNativeUi argument/wiring.
        // Main itself must acquire; the test does not prewarm or inject an image.
        AvatarImages avatars(&api,&session,&files);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences,&avatars);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Avatar window did not settle and release focus before teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->resize(1440,900); QCOMPARE(window->devicePixelRatio(),qreal(1));
        session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(),2);
        QTRY_VERIFY(controller.channelMembersVerified());
        QTRY_COMPARE(controller.viewerDisplayName(),QString("Cassi Synthetic"));
        QTRY_VERIFY(visibleItem("stream-"+windowDirectId()));
        click(window,visibleItem("stream-"+windowDirectId())); // Real existing direct sidebar action.
        QTRY_COMPARE(controller.selectedStream(),windowDirectId());
        QVERIFY(controller.selectedTopic().isEmpty());
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.directPeer().value("displayName").toString(),QString("Alice Synthetic"));
        QTRY_VERIFY(visibleItem("directConversationName"));
        QCOMPARE(visibleItem("directConversationName")->property("text").toString(),QString("Alice Synthetic"));
        QTRY_COMPARE(visibleItem("conversationHeader")->height(),qreal(56));
        QTRY_VERIFY(visibleItem("message-avatar-"+messageId));
        if (encryptedDirect) {
            // Source and unchanged Main place the locked-key footer before the
            // unencrypted aggregate topic chooser. This fixture has no vault.
            QVERIFY(!visibleItem("streamTopicPrompt"));
            QTRY_VERIFY(visibleItem("encryptedReadStatus"));
            QTRY_VERIFY(visibleItem("encryptedReadStatusText"));
            QCOMPARE(controller.encryption().value("available").toBool(),false);
            QCOMPARE(visibleItem("encryptedReadStatusText")->property("text").toString(),
                locale=="ru" ? QString("Шифрование для этого чата недоступно или не удалось открыть локальное хранилище ключей.")
                             : QString("Encryption is unavailable for this chat or local key storage could not be opened."));
        } else {
            QTRY_VERIFY(visibleItem("streamTopicPrompt"));
            QVERIFY(!visibleItem("encryptedReadStatus"));
        }
        QVERIFY(fixture.usersGets>0); QVERIFY(fixture.historyGets>0);
        QCOMPARE(fixture.users.at(0).toObject().value("avatar").toString(),"urn:image:"+ViewerAvatarFile);
        QCOMPARE(fixture.users.at(1).toObject().value("avatar").toString(),"urn:image:"+PeerAvatarFile);
        QPointer<QQuickItem> viewer=visibleItem("profileAvatar");
        QPointer<QQuickItem> sidebar=avatarProbeCircle(visibleItem("stream-"+windowDirectId()),40);
        QPointer<QQuickItem> header=avatarProbeCircle(visibleItem("directConversationHeader"),40);
        QPointer<QQuickItem> rail=visibleItem("message-avatar-"+messageId);
        QVERIFY(viewer&&sidebar&&header&&rail);
        QCOMPARE(viewer->size(),QSizeF(36,36)); QCOMPARE(sidebar->size(),QSizeF(40,40));
        QCOMPARE(header->size(),QSizeF(40,40)); QCOMPARE(rail->size(),QSizeF(48,48));
        QSignalSpy frame(window,&QQuickWindow::frameSwapped); window->update(); QTRY_VERIFY(!frame.isEmpty());
        const auto captureAvatar=[&](const QString &stage) {
            if (captures.isEmpty()) return;
            QVERIFY(QDir().mkpath(captures));
            const auto base=QDir(captures).filePath("native-avatars-"+locale+"-"+mode
                +(encryptedDirect?"-protected-direct":"-direct")+"-"+stage);
            const auto pixels=window->grabWindow(); QVERIFY(!pixels.isNull());
            QCOMPARE(pixels.size(),QSize(1440,900)); QVERIFY(pixels.save(base+".png"));
            const auto bounds=[](QQuickItem *item) {
                const auto point=item->mapToScene(QPointF{});
                return QJsonObject{{"x",point.x()},{"y",point.y()},{"width",item->width()},{"height",item->height()}};
            };
            const auto counts=[](const QMap<QString,int> &map) {
                QJsonObject result; for(auto i=map.cbegin();i!=map.cend();++i) result[i.key()]=i.value(); return result;
            };
            const QJsonObject facts{{"synthetic",true},{"liveAccountAcceptance",false},{"stage",stage},
                {"locale",preferences.locale()},{"mode",preferences.mode()},{"dpr",window->devicePixelRatio()},
                {"platform",QGuiApplication::platformName()},{"graphicsApi",int(window->rendererInterface()->graphicsApi())},
                {"stream",controller.selectedStream()},{"topic",controller.selectedTopic()},
                {"viewerName",controller.viewerDisplayName()},{"peerName",controller.directPeer().value("displayName").toString()},
                {"encryptedDirect",encryptedDirect},{"headerHeight",visibleItem("conversationHeader")->height()},
                {"topicPromptVisible",bool(visibleItem("streamTopicPrompt"))},
                {"encryptionStatusVisible",bool(visibleItem("encryptedReadStatus"))},
                {"encryptionStatusText",visibleItem("encryptedReadStatusText") ? visibleItem("encryptedReadStatusText")->property("text").toString() : QString{}},
                {"faces",QJsonObject{{"profile",bounds(viewer.data())},{"sidebar",bounds(sidebar.data())},
                    {"header",bounds(header.data())},{"rail",bounds(rail.data())}}},
                {"metadataGets",counts(metadataGets)},{"binaryGets",counts(binaryGets)},
                {"viewerPngSha256",QString::fromLatin1(QCryptographicHash::hash(viewerPng,QCryptographicHash::Sha256).toHex())},
                {"peerPngSha256",QString::fromLatin1(QCryptographicHash::hash(peerPng,QCryptographicHash::Sha256).toHex())},
                {"servedPublicCollectionResponses",servedResponses}};
            QFile receipt(base+"-metrics.json"); QVERIFY(receipt.open(QIODevice::WriteOnly));
            const auto body=QJsonDocument(facts).toJson(); QCOMPARE(receipt.write(body),qint64(body.size()));
        };
        captureAvatar("before-pixel-check");
        // First intended old-Main failure is an actual rendered RGB consequence,
        // not a missing API/property declaration or artificial image-ready flag.
        QTRY_VERIFY2(avatarProbeMatchesPixels(window,viewer,false),"Viewer avatar must show the actual center-cropped PNG");
        QTRY_VERIFY2(avatarProbeMatchesPixels(window,sidebar,true),"Direct sidebar avatar must show peer PNG");
        QTRY_VERIFY2(avatarProbeMatchesPixels(window,header,true),"Direct header avatar must show peer PNG");
        QTRY_VERIFY2(avatarProbeMatchesPixels(window,rail,true),"Peer rail avatar must show peer PNG");
        for(const auto &face:{viewer,sidebar,header,rail}) {
            auto *image=avatarProbeReadyImage(face); QVERIFY(image);
            const auto bounds=image->mapRectToItem(face,QRectF(QPointF{},image->size()));
            QCOMPARE(bounds,QRectF(1,1,face->width()-2,face->height()-2));
            QCOMPARE(QQmlProperty(face,"border.width").read().toReal(),qreal(1));
            QCOMPARE(face->property("radius").toReal(),face->width()/2);
        }
        QVERIFY(!hasVisibleText(viewer,"C")); QVERIFY(!hasVisibleText(header,"A"));
        QVERIFY(!hasVisibleText(rail,"A"));
        if(encryptedDirect) QVERIFY(!avatarProbeHasVisibleLock(sidebar));
        for(const auto &uuid:{ViewerAvatarFile,PeerAvatarFile}) {
            QCOMPARE(metadataGets.value(uuid),1); QCOMPARE(binaryGets.value(uuid),1);
        }
        for(const auto &request:fixture.server.requests) {
            if(!request.target.path().startsWith("/api/workspace/v1/messenger/files/")) continue;
            QCOMPARE(request.method,QByteArray("GET"));
            QCOMPARE(request.headers.value("authorization"),QByteArray("Bearer synthetic-project-token"));
            QVERIFY(request.headers.value("cookie").isEmpty());
        }
        // Three peer consumers must join one real cold UUID transfer. No data/key
        // transfer is injected by this test and no plaintext/private key is used.
        QCOMPARE(fixture.posts+fixture.memberPosts,0);
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        captureAvatar("ready");
        const bool closed=closeIncubatedTestWindow(engine,window);
        if(closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void init()
    {
        QTest::failOnWarning(QRegularExpression(".*(?:TypeError|ReferenceError):.*"));
        QTest::failOnWarning(QRegularExpression(".*Binding loop detected.*"));
        QTest::failOnWarning(QRegularExpression(".*items in the process of being created at engine destruction.*"));
        QTest::failOnWarning(QRegularExpression(".*(?:[Ff]ailed.*shader|shader.*[Ff]ailed|ShaderEffect.*(?:unsupported|not supported)).*"));
        QTest::failOnWarning(QRegularExpression(".*(?:Cannot open|Error decoding|Cannot load).*qrc:/workspace/.*"));
    }

    void composerFormattingDefaultsWrapSelectionAndSendCanonicalMarkdown_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("modifiers");
        QTest::addColumn<QString>("marker");
        QTest::newRow("bold") << int(Qt::Key_B) << int(Qt::ControlModifier) << QString("**");
        QTest::newRow("italic") << int(Qt::Key_I) << int(Qt::ControlModifier) << QString("*");
        QTest::newRow("code") << int(Qt::Key_E) << int(Qt::ControlModifier) << QString("`");
        QTest::newRow("strike") << int(Qt::Key_X) << int(Qt::ControlModifier | Qt::ShiftModifier) << QString("~~");
    }

    void durableComposerRestoresActualInputAcrossWindowAndControllerLifetimes()
    {
        WindowChannelFixture fixture; fixture.sourceScene = true;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [fallback](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/")
                return {QJsonArray{}};
            return fallback(request);
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString first = "  Draft from the native composer\nТекст 😀  ";
        const QString second = "Second conversation input";
        for (int lifetime = 0; lifetime < 2; ++lifetime) {
            NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
            NativeCacheService cache(directory.filePath("cache"));
            ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
            controller.useDrafts(&drafts);
            NativePreferences preferences(directory.filePath("appearance.ini")); preferences.setLocale("en");
            QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
            auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
                if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                    QTest::qFail("Draft composer teardown", __FILE__, __LINE__);
            });
            QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
            QVERIFY(startComposerEmojiFixture(fixture, controller, session));
            QVERIFY(controller.error().isEmpty());
            QTRY_VERIFY(drafts.ready(streamId, topicId));
            QPointer<QQuickItem> composer = item(window->contentItem(), "messageComposer");
            QVERIFY(composer && composer->isEnabled());
            if (lifetime == 0) {
                click(window, composer); QTRY_VERIFY(composer->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Space); QTest::keyClick(window, Qt::Key_Space);
                QTRY_COMPARE(composer->property("text").toString(), "  ");
                QCOMPARE(controller.draft(), "  ");
                QInputMethodEvent input; input.setCommitString(first.mid(2));
                QCoreApplication::sendEvent(composer, &input);
            }
            QTRY_COMPARE(composer->property("text").toString(), first);
            QCOMPARE(controller.draft(), first);
            controller.selectTopic(secondTopicId); QTRY_VERIFY(drafts.ready(streamId, secondTopicId));
            if (lifetime == 0) {
                QTRY_VERIFY(composer->property("text").toString().isEmpty());
                click(window, composer); QTRY_VERIFY(composer->hasActiveFocus());
                for (const auto character : second.toLatin1()) QTest::keyClick(window, character);
            }
            QTRY_COMPARE(composer->property("text").toString(), second);
            controller.selectTopic(topicId); QTRY_VERIFY(drafts.ready(streamId, topicId));
            QTRY_VERIFY(!controller.busy()); QVERIFY(controller.error().isEmpty());
            QTRY_COMPARE(composer->property("text").toString(), first);
            const auto captures = qEnvironmentVariable("CASSI_NATIVE_DRAFT_CAPTURE");
            if (!captures.isEmpty()) {
                QVERIFY(QDir().mkpath(captures));
                QVERIFY(window->grabWindow().save(QDir(captures).filePath(QString("composer-lifetime-%1.png").arg(lifetime))));
            }
            // Exercise the same final storage barrier called by Main for quit
            // and updater restart, including the last keystroke before debounce.
            bool done = false, okay = false;
            shutdownNativeStorage(drafts, storage, cache, this, [&](bool success) { okay = success; done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 3000); QVERIFY(okay);
            QVERIFY(closeIncubatedTestWindow(engine, window)); close.dismiss();
        }
    }

    void draftsActualMainPageRestoresExactCardAndKeepsSavingInputEditable_data()
    {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::newRow("en-light") << QString("en") << QString("light");
        QTest::newRow("ru-dark") << QString("ru") << QString("dark");
    }

    void draftsActualMainPageRestoresExactCardAndKeepsSavingInputEditable()
    {
        QFETCH(QString, locale); QFETCH(QString, mode);
        WindowDraftFixture fixture;
        const QString newerId = "78000000-0000-4000-8000-000000000001";
        fixture.drafts[returnedId] = WindowDraftFixture::row(returnedId, "Older exact draft <tag>", 1, 1);
        fixture.drafts[newerId] = WindowDraftFixture::row(newerId, "Newest conversation draft", 1, 2);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        NativeCacheService cache(directory.filePath("cache"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        controller.useDrafts(&drafts);
        NativePreferences preferences(directory.filePath("draft-page.ini"));
        preferences.setLocale(locale); preferences.setMode(mode);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Draft page teardown", __FILE__, __LINE__);
        });
        auto release = qScopeGuard([&] { fixture.finishPut(); });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(drafts.ready(streamId, topicId) && !drafts.remoteStatus().busy);
        QTRY_COMPARE(controller.draft(), QString("Newest conversation draft"));
        auto *composer = item(window->contentItem(), "messageComposer"); QVERIFY(composer);
        const auto view = [&](const QString &name) { return visibleItem(window->contentItem(), name); };
        auto *activity = view("activity-drafts"); QVERIFY(activity && activity->isEnabled());
        for (const auto *name : {"activity-favorites", "activity-starred"}) {
            auto *unsupported = view(QLatin1String(name)); QVERIFY(unsupported); QVERIFY(!unsupported->isEnabled());
        }
        QVERIFY(view("activity-mentions")->isEnabled()); // enabled by issue #42
        QVERIFY(view("activity-inbox")->isEnabled()); // enabled by issue #44
        click(window, activity);
        QTRY_VERIFY(controller.draftsVisible() && view("nativeDraftsPage"));
        QTRY_VERIFY(view("draft-card-" + returnedId) && view("draft-card-" + newerId));
        QVERIFY(!composer->isVisible()); QVERIFY(!composer->isEnabled());
        QCOMPARE(view("draft-preview-" + returnedId)->property("text").toString(), QString("Older exact draft <tag>"));
        QCOMPARE(view("draft-preview-" + returnedId)->property("textFormat").toInt(), int(Qt::PlainText));
        QVERIFY(hasVisibleText(window->contentItem(), "Product team"));
        QVERIFY(hasVisibleText(window->contentItem(), "General Topic"));
        QVERIFY(hasVisibleText(window->contentItem(), locale == "ru" ? "Черновики" : "Drafts"));
        QVERIFY(hasVisibleText(window->contentItem(), locale == "ru" ? "Канал" : "Channel"));
        const auto captures = qEnvironmentVariable("CASSI_NATIVE_DRAFT_PAGE_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(window->grabWindow().save(QDir(captures).filePath("drafts-" + locale + '-' + mode + ".png")));
        }
        click(window, view("draftsCloseButton"));
        QTRY_VERIFY(!controller.draftsVisible() && composer->isVisible());
        QCOMPARE(controller.draft(), QString("Newest conversation draft"));
        click(window, view("activity-drafts")); QTRY_VERIFY(view("draft-open-" + returnedId));
        click(window, view("draft-open-" + returnedId));
        QTRY_VERIFY(!controller.draftsVisible() && composer->isVisible() && composer->isEnabled());
        QTRY_COMPARE(composer->property("text").toString(), QString("Older exact draft <tag>"));
        QCOMPARE(drafts.capture(streamId, topicId).uuid, returnedId);

        // Hold a real conditional write, then open the same saving snapshot from
        // its real card. Navigation remains usable; conflicting delete stays off.
        fixture.holdNextPut = true;
        click(window, composer); QTRY_VERIFY(composer->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QInputMethodEvent input; input.setCommitString("Saving input remains editable");
        QCoreApplication::sendEvent(composer, &input);
        QTRY_COMPARE(controller.draft(), QString("Saving input remains editable"));
        QTRY_VERIFY_WITH_TIMEOUT(bool(fixture.pendingPut), 5000);
        click(window, view("activity-drafts"));
        QTRY_VERIFY(view("draft-open-" + returnedId) && view("draft-delete-" + returnedId));
        QVERIFY(view("draft-open-" + returnedId)->isEnabled());
        QVERIFY(view("draft-edit-" + returnedId)->isEnabled());
        QVERIFY(!view("draft-delete-" + returnedId)->isEnabled());
        QCOMPARE(view("draft-status-" + returnedId)->property("text").toString(), locale == "ru" ? QString("Сохраняется") : QString("Saving"));
        click(window, view("draft-open-" + returnedId));
        QTRY_VERIFY(!controller.draftsVisible() && composer->isVisible() && composer->isEnabled());
        QCOMPARE(controller.draft(), QString("Saving input remains editable"));
        fixture.finishPut();
        QTRY_VERIFY(!drafts.remoteStatus().busy);
        QTRY_COMPARE(drafts.remoteStatus().phase, DraftRecord::Phase::Saved);
        click(window, view("activity-drafts")); QTRY_VERIFY(controller.draftsVisible());
        click(window, view("topic-" + secondTopicId));
        QTRY_VERIFY(!controller.draftsVisible() && controller.selectedTopic() == secondTopicId && !controller.busy());
        QCOMPARE(fixture.draftPuts, 1); QCOMPARE(fixture.draftDeletes, 0);
        QCOMPARE(fixture.mutationRevisions, QVector<QByteArray>{QByteArray("\"1\"")});
        QVERIFY(fixture.ownedDraftRequests); QCOMPARE(fixture.posts, 0);
        for (const auto &request : fixture.server.requests)
            QVERIFY(request.method != "POST" || !request.target.path().endsWith("/messages/"));
        drafts.pauseRemote(true); bool stopped = false, okay = false;
        shutdownNativeStorage(drafts, storage, cache, this, [&](bool success) { okay = success; stopped = true; });
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 5000); QVERIFY(okay);
        QVERIFY(closeIncubatedTestWindow(engine, window)); close.dismiss();
    }

    // Issue #42: the sidebar Mentions chip opens the page, both collections
    // render, and card/close control the conversation view. Geometry contract:
    // workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af activity-page.ui.tsx.
    void mentionsPageChipFilterAndCloseFlow()
    {
        class WindowMentionsFixture final : public WindowChannelFixture {
        public:
            int mentionLists = 0;
            QJsonArray all;
            QJsonObject one(const QString &id, const QString &text, bool unread, int minute)
            {
                auto row = message(text, id, topicId);
                row["author_uuid"] = channelPeer;
                row["is_own"] = false;
                row["read"] = !unread;
                row["mentioned"] = true;
                row["created_at"] = QDateTime::fromString("2026-01-02T10:00:00.000Z", Qt::ISODateWithMs)
                    .addSecs(minute * 60).toString(Qt::ISODateWithMs);
                return row;
            }
            WindowMentionsFixture()
            {
                sourceScene = true;
                const auto fallback = server.handler;
                server.handler = [this, fallback](const Request &request) -> Response {
                    const QUrlQuery query(request.target);
                    if (request.target.path().endsWith("/messages/") && query.hasQueryItem("mentioned")) {
                        ++mentionLists;
                        if (query.queryItemValue("read") == "false") {
                            QJsonArray unread;
                            for (const auto &value : all)
                                if (!value.toObject().value("read").toBool()) unread.append(value);
                            return {unread};
                        }
                        return {all};
                    }
                    return fallback(request);
                };
            }
        };
        WindowMentionsFixture fixture;
        const QString readId = "77777777-7777-4777-8777-000000000001";
        const QString unreadId = "77777777-7777-4777-8777-000000000002";
        fixture.all = {fixture.one(readId, "Deploy finished", false, 30),
                       fixture.one(unreadId, "Review needed", true, 40)};
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("p.ini"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard) closeIncubatedTestWindow(engine, guard.data());
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        const auto view = [&](const QString &name) { return visibleItem(window->contentItem(), name); };
        auto *chip = view("activity-mentions"); QVERIFY(chip && chip->isEnabled());
        click(window, chip);
        QTRY_VERIFY(controller.mentionsVisible() && view("nativeMentionsPage"));
        QTRY_VERIFY(view("mention-card-" + unreadId) && view("mention-card-" + readId));
        auto *metaProbe = visibleItem(window->contentItem(), "mention-meta-" + unreadId);
        QVERIFY(metaProbe);
        QCOMPARE(metaProbe->property("text").toString(), QString("Product team · General Topic"));
        QVERIFY(view("mention-unread-dot-" + unreadId));
        QVERIFY(!view("mention-unread-dot-" + readId));
        QVERIFY(hasVisibleText(window->contentItem(), "Review needed"));
        click(window, view("mentionsFilterUnreadButton"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.mentionsFilter() == "unread"
            && view("mention-card-" + unreadId) && !view("mention-card-" + readId), 5000);
        click(window, view("mentionsCloseButton"));
        QTRY_VERIFY(!controller.mentionsVisible());
        auto *composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isVisible() && composer->isEnabled());
        // Reopening keeps the chosen filter (page remount refetch in the reference).
        click(window, view("activity-mentions"));
        QTRY_VERIFY(controller.mentionsVisible());
        QTRY_VERIFY_WITH_TIMEOUT(view("mention-card-" + unreadId) && !view("mention-card-" + readId), 5000);
        click(window, view("mention-card-" + unreadId));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.mentionsVisible(), 5000);
        QVERIFY(composer->isVisible() && composer->isEnabled());
        QVERIFY(fixture.mentionLists >= 2);
    }

    // Issue #44: the sidebar Inbox chip opens the page, stream cards render
    // with badges, a topic row selects its conversation and unmounts the
    // page, and closing restores the conversation column. Geometry contract:
    // workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af inbox-page.ui.tsx.
    void inboxPageChipRowsAndCloseFlow()
    {
        class WindowInboxFixture final : public WindowChannelFixture {
        public:
            WindowInboxFixture()
            {
                sourceScene = true;
                const auto fallback = server.handler;
                server.handler = [this, fallback](const Request &request) -> Response {
                    const auto path = request.target.path();
                    if (path.endsWith("/streams/") && request.method == "GET") {
                        auto dm = windowChannel(dmStreamId, "Alice");
                        dm["private"] = true;
                        dm["direct_user_uuid"] = channelPeer;
                        dm["unread_count"] = 5;
                        dm["active_unread_count"] = 5;
                        auto team = windowChannel(streamId, "Product team");
                        team["default_topic_uuid"] = topicId;
                        team["unread_count"] = 3;
                        team["active_unread_count"] = 3;
                        return {QJsonArray{team, dm}};
                    }
                    if (path.endsWith("/stream_topics/")) {
                        const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                        if (stream == streamId) {
                            // Same scene as the base fixture, but Releases
                            // carries active unreads so it surfaces in the inbox.
                            auto releases = windowChannelTopic(
                                "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", streamId, "Releases");
                            releases["is_default"] = false;
                            releases["is_done"] = true;
                            releases["unread_count"] = 3;
                            releases["active_unread_count"] = 3;
                            auto planning = windowChannelTopic(secondTopicId, streamId, "Planning");
                            planning["is_default"] = false;
                            return {QJsonArray{
                                windowChannelTopic(topicId, streamId, "General Topic"),
                                planning, releases}};
                        }
                        if (stream == dmStreamId) {
                            auto dmTopic = windowChannelTopic(dmTopicId, dmStreamId, "");
                            dmTopic["is_default"] = false;
                            dmTopic["unread_count"] = 5;
                            dmTopic["active_unread_count"] = 5;
                            return {QJsonArray{dmTopic}};
                        }
                    }
                    return fallback(request);
                };
            }
            QString dmStreamId = "a2000000-0000-4000-8000-000000000002";
            QString dmTopicId = "b2000000-0000-4000-8000-000000000002";
        };
        WindowInboxFixture fixture;
        // The source scene's unread topic: Releases carries active_unread 3.
        const QString releasesId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("p.ini"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard) closeIncubatedTestWindow(engine, guard.data());
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        const auto view = [&](const QString &name) { return visibleItem(window->contentItem(), name); };
        auto *chip = view("activity-inbox"); QVERIFY(chip && chip->isEnabled());
        QVERIFY(view("messageComposer"));
        click(window, chip);
        QTRY_VERIFY(controller.inboxVisible() && view("nativeInboxPage"));
        QVERIFY(!view("messageComposer")); // conversation column hidden
        QTRY_VERIFY_WITH_TIMEOUT(view("inbox-card-" + streamId)
            && view("inbox-card-" + fixture.dmStreamId), 5000);
        // Ordering: direct-private cards lead the direct section (ui.tsx:36).
        auto *dmCard = view("inbox-card-" + fixture.dmStreamId);
        auto *channelCard = view("inbox-card-" + streamId);
        QVERIFY(dmCard && channelCard);
        QVERIFY(dmCard->mapToItem(window->contentItem(), QPointF{}).y()
            < channelCard->mapToItem(window->contentItem(), QPointF{}).y());
        auto *channelBadge = view("inbox-card-badge-" + streamId); QVERIFY(channelBadge);
        QCOMPARE(channelBadge->property("text").toString(), QString("3"));
        auto *dmBadge = view("inbox-card-badge-" + fixture.dmStreamId); QVERIFY(dmBadge);
        QCOMPARE(dmBadge->property("text").toString(), QString("5"));
        // Topic rows refine once per-stream catalogs land (only the
        // active-unread topics surface; General/Planning stay out).
        QTRY_VERIFY_WITH_TIMEOUT(view("inbox-row-" + releasesId)
            && view("inbox-row-" + fixture.dmTopicId), 5000);
        QVERIFY(!view("inbox-row-" + topicId));
        // Empty-topic DM row: "peer · All messages" per topicRow's
        // `${streamTitle} · ${topicTitle}` with the allMessages fallback (ui.tsx:59-71).
        auto *dmTitle = view("inbox-row-title-" + fixture.dmTopicId); QVERIFY(dmTitle);
        QCOMPARE(dmTitle->property("text").toString(), QString("Alice · All messages"));
        // Clicking a DM topic row opens the conversation and closes the page.
        // (The Releases row would select a resolved topic, whose composer the
        // reference hides — that is existing native behaviour, not inbox.)
        click(window, view("inbox-row-" + fixture.dmTopicId));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.inboxVisible()
            && controller.selectedStream() == fixture.dmStreamId
            && controller.selectedTopic() == fixture.dmTopicId, 5000);
        QVERIFY(view("messageComposer"));
        // The close control returns to the conversation without navigation.
        click(window, chip);
        QTRY_VERIFY(controller.inboxVisible());
        click(window, view("inboxCloseButton"));
        QVERIFY(!controller.inboxVisible());
        QVERIFY(view("messageComposer"));
    }

    // Issue #54: switching sidebar destinations in the mounted window keeps
    // exactly one page visible and exactly one sidebar entry selected, and
    // closing the active page restores the conversation column. Draft card
    // content survives Drafts -> Mentions -> Drafts.
    void draftsMentionsSidebarNavigationKeepsOnePageActive()
    {
        class WindowDraftMentionsFixture final : public WindowDraftFixture {
        public:
            QJsonArray mentions;
            WindowDraftMentionsFixture()
            {
                const auto fallback = server.handler;
                server.handler = [this, fallback](const Request &request) -> Response {
                    const QUrlQuery query(request.target);
                    if (request.target.path().endsWith("/messages/") && query.hasQueryItem("mentioned"))
                        return {mentions};
                    return fallback(request);
                };
            }
        };
        WindowDraftMentionsFixture fixture;
        fixture.drafts[returnedId] = WindowDraftFixture::row(returnedId, "Kept across navigation", 1, 1);
        const QString mentionId = "75000000-0000-4500-8500-000000000001";
        auto mention = message("Deploy finished", mentionId, topicId);
        mention["author_uuid"] = channelPeer;
        mention["is_own"] = false;
        mention["read"] = false;
        mention["mentioned"] = true;
        fixture.mentions = {mention};
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        NativeCacheService cache(directory.filePath("cache"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        controller.useDrafts(&drafts);
        NativePreferences preferences(directory.filePath("nav-exclusivity.ini"));
        preferences.setLocale("en");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard) closeIncubatedTestWindow(engine, guard.data());
        });
        auto release = qScopeGuard([&] { fixture.finishPut(); });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(drafts.ready(streamId, topicId) && !drafts.remoteStatus().busy);
        const auto view = [&](const QString &name) { return visibleItem(window->contentItem(), name); };
        const auto selected = [&](const QString &id) {
            auto *chip = view("activity-" + id);
            return chip && chip->property("selected").toBool();
        };
        const auto page = [&](const QString &name) { return item(window->contentItem(), name); };
        QVERIFY(view("activity-drafts") && view("activity-mentions")->isEnabled()
            && view("activity-inbox")->isEnabled());
        // Drafts opens alone with exactly the drafts entry selected.
        click(window, view("activity-drafts"));
        QTRY_VERIFY(controller.draftsVisible() && view("nativeDraftsPage"));
        QTRY_VERIFY(view("draft-card-" + returnedId));
        QVERIFY(selected("drafts"));
        QVERIFY(!selected("mentions") && !selected("inbox"));
        // Clicking Mentions replaces the drafts page: one page, one selection.
        click(window, view("activity-mentions"));
        QTRY_VERIFY(controller.mentionsVisible() && view("nativeMentionsPage"));
        QVERIFY(!controller.draftsVisible());
        QVERIFY(!page("nativeDraftsPage") || !page("nativeDraftsPage")->isVisible());
        QVERIFY(selected("mentions"));
        QVERIFY(!selected("drafts") && !selected("inbox"));
        // Back to Drafts: the page replaces Mentions and the draft card survived.
        click(window, view("activity-drafts"));
        QTRY_VERIFY(controller.draftsVisible() && view("nativeDraftsPage"));
        QVERIFY(!controller.mentionsVisible());
        QVERIFY(!page("nativeMentionsPage") || !page("nativeMentionsPage")->isVisible());
        QTRY_VERIFY(view("draft-card-" + returnedId));
        QCOMPARE(controller.draft(), QString("Kept across navigation"));
        // Inbox also replaces the active page.
        click(window, view("activity-inbox"));
        QTRY_VERIFY(controller.inboxVisible() && view("nativeInboxPage"));
        QVERIFY(!controller.draftsVisible());
        QVERIFY(selected("inbox"));
        QVERIFY(!selected("drafts") && !selected("mentions"));
        // Closing the active page restores the conversation with no selection.
        click(window, view("inboxCloseButton"));
        QTRY_VERIFY(!controller.inboxVisible() && !controller.draftsVisible()
            && !controller.mentionsVisible());
        QVERIFY(view("messageComposer"));
        QVERIFY(!selected("inbox") && !selected("drafts") && !selected("mentions"));
        drafts.pauseRemote(true); bool stopped = false, okay = false;
        shutdownNativeStorage(drafts, storage, cache, this, [&](bool success) { okay = success; stopped = true; });
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 5000); QVERIFY(okay);
        QVERIFY(closeIncubatedTestWindow(engine, window)); close.dismiss();
    }

    void draftsActualMainConflictButtonsUseConditionalOwnedActions_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("server") << QString("server");
        QTest::newRow("local") << QString("local");
        QTest::newRow("delete") << QString("delete");
    }

    void draftsActualMainConflictButtonsUseConditionalOwnedActions()
    {
        QFETCH(QString, action); WindowDraftFixture fixture;
        fixture.drafts[returnedId] = WindowDraftFixture::row(returnedId, "Initial server draft");
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        NativeCacheService cache(directory.filePath("cache"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        controller.useDrafts(&drafts);
        NativePreferences preferences(directory.filePath("draft-conflict.ini")); preferences.setLocale("en");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Draft conflict teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(drafts.ready(streamId, topicId) && !drafts.remoteStatus().busy);
        QTRY_COMPARE(controller.draft(), QString("Initial server draft"));
        fixture.drafts[returnedId] = WindowDraftFixture::row(returnedId, "Changed on another client", 2, 3);
        const auto view = [&](const QString &name) { return visibleItem(window->contentItem(), name); };
        auto *composer = view("messageComposer"); QVERIFY(composer);
        click(window, composer); QTRY_VERIFY(composer->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QInputMethodEvent input; input.setCommitString("My conflicting draft"); QCoreApplication::sendEvent(composer, &input);
        QTRY_VERIFY_WITH_TIMEOUT(drafts.remoteStatus().conflict, 5000);
        QCOMPARE(controller.draft(), QString("My conflicting draft"));
        click(window, view("activity-drafts"));
        QTRY_VERIFY(view("draft-use-server-" + returnedId) && view("draft-keep-local-" + returnedId)
            && view("draft-delete-server-" + returnedId));
        QCOMPARE(view("draft-preview-" + returnedId)->property("text").toString(), QString("My conflicting draft"));
        QCOMPARE(view("draft-status-" + returnedId)->property("text").toString(), QString("Conflict: remote version was kept"));
        QVERIFY(!view("draft-delete-" + returnedId));
        const auto captures = qEnvironmentVariable("CASSI_NATIVE_DRAFT_PAGE_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(window->grabWindow().save(QDir(captures).filePath("draft-conflict-" + action + ".png")));
        }
        const auto button = action == "server" ? "draft-use-server-" : action == "local" ? "draft-keep-local-" : "draft-delete-server-";
        click(window, view(QLatin1String(button) + returnedId));
        if (action == "delete") {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.draftDeletes, 1, 5000);
            QTRY_VERIFY(!fixture.drafts.contains(returnedId));
            QTRY_VERIFY(!view("draft-card-" + returnedId));
            QTRY_VERIFY(hasVisibleText(window->contentItem(), "No drafts yet"));
            QCOMPARE(fixture.draftPuts, 1);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(drafts.remoteStatus().phase, DraftRecord::Phase::Saved, 5000);
            const auto expected = action == "server" ? QString("Changed on another client") : QString("My conflicting draft");
            QCOMPARE(fixture.draftPuts, action == "server" ? 1 : 2); QCOMPARE(fixture.draftDeletes, 0);
            QCOMPARE(fixture.drafts[returnedId].value("payload").toObject().value("content").toString(), expected);
            QTRY_VERIFY(view("draft-open-" + returnedId)); click(window, view("draft-open-" + returnedId));
            QTRY_VERIFY(!controller.draftsVisible() && composer->isVisible());
            QTRY_COMPARE(composer->property("text").toString(), expected);
            QCOMPARE(drafts.capture(streamId, topicId).uuid, returnedId);
        }
        QCOMPARE(fixture.mutationRevisions.first(), QByteArray("\"1\""));
        if (action != "server") QCOMPARE(fixture.mutationRevisions.last(), QByteArray("\"2\""));
        QVERIFY(fixture.ownedDraftRequests); QCOMPARE(fixture.posts, 0);
        for (const auto &request : fixture.server.requests)
            QVERIFY(request.method != "POST" || !request.target.path().endsWith("/messages/"));
        drafts.pauseRemote(true); bool stopped = false, okay = false;
        shutdownNativeStorage(drafts, storage, cache, this, [&](bool success) { okay = success; stopped = true; });
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 5000); QVERIFY(okay);
        QVERIFY(closeIncubatedTestWindow(engine, window)); close.dismiss();
    }

    void composerFormattingDefaultsWrapSelectionAndSendCanonicalMarkdown()
    {
        QFETCH(int, key); QFETCH(int, modifiers); QFETCH(QString, marker);
        WindowChannelFixture fixture; fixture.sourceScene = true;
        QJsonObject posted, saved; int posts = 0, confirmations = 0;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path == "/api/workspace/v1/messenger/messages/") {
                ++posts; posted = request.body;
                const auto id = QUuid::createUuidV5(QUuid(topicId),
                    posted.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved = message(posted.value("payload").toObject().value("content").toString(), id);
                saved["user_uuid"] = userId; saved["updated_at"] = saved.value("created_at");
                return {saved, 201};
            }
            if (request.method == "GET" && !saved.isEmpty()
                && path == "/api/workspace/v1/messenger/messages/" + saved.value("uuid").toString()) {
                ++confirmations; return {saved};
            }
            if (request.method == "GET" && path == "/api/workspace/v1/messenger/messages/")
                return {QJsonArray{}};
            return fallback(request);
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("composer-formatting.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Composer formatting teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QPointer<QQuickItem> composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isEnabled()); QCOMPARE(controller.selectedTopic(), topicId);
        click(window, composer.data()); QTRY_VERIFY(composer->hasActiveFocus());
        for (const auto character : QByteArray("before word after")) QTest::keyClick(window, character);
        QTRY_COMPARE(controller.draft(), QString("before word after"));
        QTest::keyClick(window, Qt::Key_Home);
        for (int index = 0; index < 7; ++index) QTest::keyClick(window, Qt::Key_Right);
        for (int index = 0; index < 4; ++index) QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(composer->property("selectedText").toString(), QString("word"));
        QCOMPARE(composer->property("selectionStart").toInt(), 7);
        QCOMPARE(composer->property("selectionEnd").toInt(), 11); QCOMPARE(posts, 0);
        QTest::keyClick(window, Qt::Key(key), Qt::KeyboardModifiers(modifiers));
        const auto selected = "before " + marker + "word" + marker + " after";
        // First intended RED: the unchanged Main has no formatting action.
        QTRY_COMPARE(composer->property("text").toString(), selected);
        QCOMPARE(controller.draft(), selected); QTRY_VERIFY(composer->hasActiveFocus());
        QCOMPARE(composer->property("selectionStart").toInt(), 11 + 2 * marker.size());
        QCOMPARE(composer->property("selectionEnd").toInt(), 11 + 2 * marker.size());
        QCOMPARE(composer->property("cursorPosition").toInt(), 11 + 2 * marker.size());
        QCOMPARE(posts, 0); QCOMPARE(confirmations, 0);

        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const auto character : QByteArray("abcd")) QTest::keyClick(window, character);
        QTRY_COMPARE(controller.draft(), QString("abcd"));
        QTest::keyClick(window, Qt::Key_Home); QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key(key), Qt::KeyboardModifiers(modifiers));
        QTRY_COMPARE(controller.draft(), "ab" + marker + marker + "cd");
        QCOMPARE(composer->property("cursorPosition").toInt(), 2 + marker.size());
        QCOMPARE(composer->property("selectionStart").toInt(), composer->property("selectionEnd").toInt());
        for (const auto character : QByteArray("inside")) QTest::keyClick(window, character);
        const auto sentText = "ab" + marker + "inside" + marker + "cd";
        QTRY_COMPARE(controller.draft(), sentText); QCOMPARE(posts, 0);
        QSignalSpy sent(&controller, &NativeController::messageSent);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(posts, 1); QTRY_COMPARE(confirmations, 1); QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(posted.value("stream_uuid").toString(), streamId);
        QCOMPARE(posted.value("topic_uuid").toString(), topicId);
        QCOMPARE(posted.value("payload").toObject(), (QJsonObject{{"kind", "markdown"}, {"content", sentText}}));
        const auto canonical = QUuid::createUuidV5(QUuid(topicId),
            posted.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(sent.first().at(0).toString(), canonical);
        QCOMPARE(saved.value("uuid").toString(), canonical);
        QTRY_COMPARE(controller.draft(), QString{});
        QTRY_VERIFY(item(window->contentItem(), "message-content-" + canonical));
        QVERIFY(item(window->contentItem(), "message-content-" + canonical)->property("text").toString().contains("inside"));
        QCOMPARE(posts, 1); QCOMPARE(confirmations, 1); QCOMPARE(fixture.memberPosts, 0);
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const bool closed = closeIncubatedTestWindow(engine, window); if (closed) close.dismiss(); QVERIFY(closed);
    }

    void composerFormattingDefaultsRespectCompositionRepeatAndOtherTargets()
    {
        WindowChannelFixture fixture; fixture.sourceScene = true;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("formatting-guards.ini"));
        preferences.setLocale("ru"); preferences.setMode("dark");
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Formatting input guards teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QPointer<QQuickItem> composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isEnabled()); click(window, composer.data());
        QTRY_VERIFY(composer->hasActiveFocus());
        for (const auto character : QByteArray("safe")) QTest::keyClick(window, character);
        QTRY_COMPARE(controller.draft(), QString("safe"));
        // Actual public input-method delivery, not a composing-property override.
        QInputMethodEvent preedit(QString::fromUtf8("комп"), {});
        QVERIFY(QCoreApplication::sendEvent(window->focusObject(), &preedit));
        QTRY_VERIFY(composer->property("inputMethodComposing").toBool());
        QCOMPARE(composer->property("preeditText").toString(), QString::fromUtf8("комп"));
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(controller.draft(), QString("safe"));
        QInputMethodEvent commit; commit.setCommitString(" ime");
        QVERIFY(QCoreApplication::sendEvent(window->focusObject(), &commit));
        QTRY_VERIFY(!composer->property("inputMethodComposing").toBool());
        QTRY_COMPARE(controller.draft(), QString("safe ime"));
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        // Same QtTest window-system path as keyClick, including autorepeat.
        QTest::simulateEvent(window, true, Qt::Key_B, Qt::ControlModifier, "b", true);
        QTest::simulateEvent(window, false, Qt::Key_B, Qt::ControlModifier, "b", true);
        QCOMPARE(controller.draft(), QString("safe ime"));
        const auto altGraph = Qt::ControlModifier | Qt::AltModifier | Qt::GroupSwitchModifier;
        QTest::simulateEvent(window, true, Qt::Key_B, altGraph, {}, false);
        QTest::simulateEvent(window, false, Qt::Key_B, altGraph, {}, false);
        QCOMPARE(controller.draft(), QString("safe ime"));
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(controller.draft(), QString("safe ime"));

        auto *search = item(window->contentItem(), "sidebarSearch"); QVERIFY(search);
        click(window, search); QTRY_VERIFY(search->hasActiveFocus());
        for (const auto character : QByteArray("other")) QTest::keyClick(window, character);
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(search->property("text").toString(), QString("other"));
        QCOMPARE(controller.draft(), QString("safe ime"));
        click(window, item(window->contentItem(), "accountMenuButton"));
        QTRY_VERIFY(visibleItem("ownStatusAction")); click(window, visibleItem("ownStatusAction"));
        QTRY_VERIFY(visibleItem("ownStatusText"));
        auto *status = visibleItem("ownStatusText"); click(window, status);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const auto character : QByteArray("modal")) QTest::keyClick(window, character);
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(status->property("text").toString(), QString("modal"));
        QCOMPARE(controller.draft(), QString("safe ime"));
        click(window, visibleItem("ownStatusCancel")); QTRY_VERIFY(!visibleItem("ownStatusText"));
        click(window, composer.data()); QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        // A nonrepeat delivered by the SAME helper must positively format.
        // This is controlled Qt input, not proof of an OS keyboard layout.
        QTest::simulateEvent(window, true, Qt::Key_B, Qt::ControlModifier, "b", false);
        QTest::simulateEvent(window, false, Qt::Key_B, Qt::ControlModifier, "b", false);
        QTRY_COMPARE(controller.draft(), QString("**safe ime**"));
        QCOMPARE(composer->property("cursorPosition").toInt(), 12);
        QCOMPARE(fixture.posts, 0); QCOMPARE(fixture.memberPosts, 0);
        for (const auto &request : fixture.server.requests)
            if (request.method != "GET") QVERIFY(!request.target.path().startsWith("/api/workspace/"));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const bool closed = closeIncubatedTestWindow(engine, window); if (closed) close.dismiss(); QVERIFY(closed);
    }

    void composerFormattingDefaultsPreserveUtf16MultilineAndScroll()
    {
        WindowChannelFixture fixture; fixture.sourceScene = true;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("formatting-scroll.ini"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Formatting multiline teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QPointer<QQuickItem> composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isEnabled()); click(window, composer.data());
        QString text;
        for (int index = 0; index < 30; ++index) text += QString::fromUtf8("Строка 😀 %1\n").arg(index);
        text += QString::fromUtf8("before 😀\nword");
        QInputMethodEvent commit; commit.setCommitString(text);
        QVERIFY(QCoreApplication::sendEvent(window->focusObject(), &commit));
        QTRY_COMPARE(controller.draft(), text);
        QTest::keyClick(window, Qt::Key_End, Qt::ControlModifier);
        QPointer<QQuickItem> flick = composer->parentItem();
        while (flick && flick->metaObject()->indexOfProperty("contentY") < 0) flick = flick->parentItem();
        QVERIFY(flick); QTRY_VERIFY(flick->property("contentY").toReal() > 0);
        // Select across the actual newline and supplementary-plane character.
        QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
        QTest::keyClick(window, Qt::Key_Home, Qt::ShiftModifier);
        const int start = composer->property("selectionStart").toInt();
        const int end = composer->property("selectionEnd").toInt();
        const auto selected = text.mid(start, end - start);
        QVERIFY(selected.contains(QChar('\n'))); QVERIFY(selected.contains(QString::fromUtf8("😀")));
        QCOMPARE(composer->property("selectedText").toString().replace(QChar::ParagraphSeparator, QChar('\n')), selected);
        QSignalSpy selectedFrame(window, &QQuickWindow::frameSwapped); QVERIFY(selectedFrame.isValid());
        window->update(); QTRY_VERIFY(!selectedFrame.isEmpty());
        const auto scrollY = flick->property("contentY").toReal();
        const auto expected = text.left(start) + "*" + text.mid(start, end - start) + "*" + text.mid(end);
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QTRY_COMPARE(controller.draft(), expected); QCOMPARE(composer->property("text").toString(), expected);
        QCOMPARE(composer->property("cursorPosition").toInt(), end + 2);
        QCOMPARE(composer->property("selectionStart").toInt(), composer->property("selectionEnd").toInt());
        QTRY_VERIFY(composer->hasActiveFocus());
        QSignalSpy frame(window, &QQuickWindow::frameSwapped); QVERIFY(frame.isValid());
        window->update(); QTRY_VERIFY(!frame.isEmpty());
        QVERIFY(flick);
        QVERIFY2(qAbs(flick->property("contentY").toReal() - scrollY) <= 0.5, "Pinned Source preserves the current textarea scroll while restoring the caret");
        QCOMPARE(fixture.posts, 0); QCOMPARE(fixture.memberPosts, 0);
        const bool closed = closeIncubatedTestWindow(engine, window); if (closed) close.dismiss(); QVERIFY(closed);
    }

    void composerFormattingDefaultsFenceSynchronousRouteAndOwnerReplacement_data()
    {
        QTest::addColumn<bool>("logout");
        QTest::newRow("route") << false;
        QTest::newRow("logout") << true;
    }

    void composerFormattingDefaultsFenceSynchronousRouteAndOwnerReplacement()
    {
        QFETCH(bool, logout);
        WindowChannelFixture fixture; fixture.sourceScene = true;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("formatting-lifetime.ini"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Formatting runtime fence teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QPointer<QQuickItem> composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isEnabled()); click(window, composer.data());
        for (const auto character : QByteArray("retained")) QTest::keyClick(window, character);
        QTRY_COMPARE(controller.draft(), QString("retained"));
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        bool replaced = false;
        const auto connection = connect(&controller, &NativeController::draftChanged, &controller, [&] {
            if (replaced || controller.draft() != "**retained**") return;
            replaced = true;
            if (logout) session.logout();
            else controller.selectTopic(secondTopicId);
        });
        auto disconnectDraft = qScopeGuard([&] { disconnect(connection); });
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(replaced); disconnect(connection); disconnectDraft.dismiss();
        QTRY_COMPARE(controller.draft(), QString{});
        if (logout) {
            QTRY_VERIFY(!controller.authenticated()); QTRY_VERIFY(!composer || !composer->isVisible());
        } else {
            QTRY_COMPARE(controller.selectedTopic(), secondTopicId); QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(composer->property("text").toString(), QString{});
            QCOMPARE(composer->property("cursorPosition").toInt(), 0);
            controller.selectTopic(topicId); QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(controller.draft(), QString("**retained**"));
        }
        QCOMPARE(fixture.posts, 0); QCOMPARE(fixture.memberPosts, 0);
        const bool closed = closeIncubatedTestWindow(engine, window); if (closed) close.dismiss(); QVERIFY(closed);
    }

    void composerEmojiAppendsUnicodeAndSendsFromTheActualMain_data()
    {
        QTest::addColumn<QString>("locale");QTest::addColumn<QString>("mode");
        QTest::newRow("en-light")<<QString("en")<<QString("light");
        QTest::newRow("ru-dark")<<QString("ru")<<QString("dark");
    }
    void composerEmojiAppendsUnicodeAndSendsFromTheActualMain()
    {
        QFETCH(QString,locale);QFETCH(QString,mode);
        WindowChannelFixture fixture;fixture.sourceScene=true;
        QJsonObject posted,saved;int posts=0,confirmations=0;
        const auto fallback=fixture.server.handler;
        fixture.server.handler=[&,fallback](const Request &request)->Response {
            const auto path=request.target.path();
            if(request.method=="POST"&&path=="/api/workspace/v1/messenger/messages/") {
                ++posts;posted=request.body;
                const auto id=QUuid::createUuidV5(QUuid(topicId),posted.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
                saved=message(posted.value("payload").toObject().value("content").toString(),id);
                saved["user_uuid"]=userId;saved["updated_at"]=saved.value("created_at");return {saved,201};
            }
            if(request.method=="GET"&&!saved.isEmpty()&&path=="/api/workspace/v1/messenger/messages/"+saved.value("uuid").toString()) {
                ++confirmations;return {saved};
            }
            if(request.method=="GET"&&path=="/api/workspace/v1/messenger/messages/")return {QJsonArray{}};
            return fallback(request);
        };
        QTemporaryDir directory;QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("composer-emoji.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Composer emoji teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture,controller,session));
        preferences.setLocale(locale);preferences.setMode(mode);
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        auto *composer=item(window->contentItem(),"messageComposer");QVERIFY(composer&&composer->isEnabled());
        QVERIFY(!controller.reactionChoices().isEmpty());QVERIFY(!controller.ownStatusContextKey().isEmpty());
        click(window,composer);for(const auto key:QByteArray("Draft "))QTest::keyClick(window,key);
        QTRY_COMPARE(controller.draft(),QString("Draft "));QVERIFY(controller.canSend());
        // Actual selection is deliberately not replaced: pinned Source appends.
        QTest::keyClick(window,Qt::Key_Home);QTest::keyClick(window,Qt::Key_Right,Qt::ShiftModifier);
        QCOMPARE(composer->property("selectedText").toString(),QString("D"));
        auto *trigger=composerEmojiTrigger(window->contentItem());QVERIFY(trigger);
        QCOMPARE(posts,0);QCOMPARE(fixture.memberPosts,0);
        // First intended capability RED on real unchanged product: disabled action.
        QVERIFY(trigger->isEnabled());click(window,trigger);
        QTRY_VERIFY(composerEmojiContent(window));
        QPointer<QQuickItem> picker=composerEmojiContent(window);QVERIFY(picker);
        auto *search=item(picker.data(),"reactionSearch");QVERIFY(search);
        captureComposerEmojiFrame(window,locale,mode,"initial");
        click(window,search);for(const auto key:QByteArray("grinning"))QTest::keyClick(window,key);
        QTRY_VERIFY(composerEmojiChoiceReady(window,picker.data()));
        captureComposerEmojiFrame(window,locale,mode,"search");
        click(window,item(picker.data(),"emoji-choice-grinning"));
        const QString once=QString::fromUtf8("Draft 😀"),twice=QString::fromUtf8("Draft 😀😀");
        QTRY_COMPARE(controller.draft(),once);QTRY_VERIFY(composer->hasActiveFocus());
        QCOMPARE(composer->property("cursorPosition").toInt(),once.size());
        QCOMPARE(composer->property("selectionStart").toInt(),composer->property("selectionEnd").toInt());
        QVERIFY(picker&&picker->isVisible());QCOMPARE(posts,0);
        QTRY_VERIFY(composerEmojiChoiceReady(window,picker.data()));
        click(window,item(picker.data(),"emoji-choice-grinning"));
        QTRY_COMPARE(controller.draft(),twice);QTRY_VERIFY(composer->hasActiveFocus());
        QCOMPARE(composer->property("cursorPosition").toInt(),twice.size());QVERIFY(picker&&picker->isVisible());
        QTest::keyClick(window,Qt::Key_Period);QTRY_COMPARE(controller.draft(),twice+".");QCOMPARE(posts,0);
        // Keep the popup open: a real Return from the refocused composer sends
        // and only the matching successful confirmation closes this picker.
        QSignalSpy sent(&controller,&NativeController::messageSent);QTest::keyClick(window,Qt::Key_Return);
        QTRY_COMPARE(posts,1);QTRY_COMPARE(confirmations,1);QTRY_COMPARE(sent.size(),1);
        QTRY_VERIFY(!composerEmojiContent(window));
        QCOMPARE(posted.value("stream_uuid").toString(),streamId);QCOMPARE(posted.value("topic_uuid").toString(),topicId);
        QCOMPARE(posted.value("payload").toObject(),(QJsonObject{{"kind","markdown"},{"content",twice+"."}}));
        const auto canonical=QUuid::createUuidV5(QUuid(topicId),posted.value("uuid").toString().toUtf8()).toString(QUuid::WithoutBraces);
        QCOMPARE(saved.value("uuid").toString(),canonical);QCOMPARE(sent.first().at(0).toString(),canonical);
        QTRY_COMPARE(controller.draft(),QString{});QTRY_VERIFY(item(window->contentItem(),"message-content-"+canonical));
        QVERIFY(item(window->contentItem(),"message-content-"+canonical)->property("text").toString().contains(twice));
        QCOMPARE(posts,1);QCOMPARE(fixture.memberPosts,0);
        for(const auto &request:fixture.server.requests)
            if(request.method!="GET")QVERIFY(!request.target.path().contains("/reactions")&&!request.target.path().contains("/presence/")&&!request.target.path().contains("/files/"));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }

    void composerEmojiKeepsHarmlessUsersButClosesOnRealRouteAndApiLifetime()
    {
        WindowChannelFixture fixture;fixture.sourceScene=true;int tokenSerial=0;
        const auto fallback=fixture.server.handler;
        fixture.server.handler=[&,fallback](const Request &request)->Response {
            auto response=fallback(request);
            if(request.target.path().endsWith("/actions/get_token/invoke")&&request.body.value("grant_type")=="refresh_token") {
                auto token=response.data.toObject();token["access_token"]=QString("synthetic-composer-token-%1").arg(++tokenSerial);response.data=token;
            }
            if(request.method=="GET"&&request.target.path()=="/api/workspace/v1/messenger/messages/")response.data=QJsonArray{};
            return response;
        };
        QTemporaryDir directory;NativePreferences preferences(directory.filePath("composer-lifetime.ini"));
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);
        QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);
        auto close=qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)]{
            if(guard&&!closeIncubatedTestWindow(engine,guard.data()))QTest::qFail("Composer lifetime teardown",__FILE__,__LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));window->requestActivate();QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture,controller,session));QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        auto *composer=item(window->contentItem(),"messageComposer");QVERIFY(composer&&composer->isEnabled());
        click(window,composer);for(const auto key:QByteArray("Retained draft"))QTest::keyClick(window,key);
        auto *trigger=composerEmojiTrigger(window->contentItem());QVERIFY(trigger);QVERIFY(trigger->isEnabled());
        click(window,trigger);QTRY_VERIFY(composerEmojiContent(window));
        QPointer<QQuickItem> picker=composerEmojiContent(window);QVERIFY(picker);
        const auto context=controller.ownStatusContextKey();const auto generation=api.generation();
        auto user=QJsonObject{{"uuid",userId},{"project_id",projectId},{"username","cassi"},{"first_name","Cassi"},{"last_name","Updated"},
            {"email","cassi@example.invalid"},{"avatar",QJsonValue::Null},{"source","iam"},{"status","active"},
            {"status_text",""},{"status_emoji",""},{"last_ping_at","2026-01-02T10:02:00Z"},
            {"created_at","2026-01-02T10:00:00Z"},{"updated_at","2026-01-02T10:02:00Z"},{"kind","user.updated"}};
        QVERIFY(controller.applyRealtimeEvent({{"uuid",QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"project_id",projectId},{"user_uuid",userId},{"payload",user}}));
        QTRY_COMPARE(controller.viewerDisplayName(),QString("Cassi Updated"));
        QCOMPARE(controller.ownStatusContextKey(),context);QCOMPARE(api.generation(),generation);
        QVERIFY(picker&&picker->isVisible());QCOMPARE(controller.draft(),QString("Retained draft"));
        // Real public route admission, not a chosen()/notify or private QML setter.
        controller.selectTopic(secondTopicId);QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedTopic(),secondTopicId);QTRY_VERIFY(!composerEmojiContent(window));
        QCOMPARE(controller.draft(),QString{});
        controller.selectTopic(topicId);QTRY_VERIFY(!controller.busy());QCOMPARE(controller.draft(),QString("Retained draft"));
        trigger=composerEmojiTrigger(window->contentItem());QVERIFY(trigger&&trigger->isEnabled());click(window,trigger);
        QTRY_VERIFY(composerEmojiContent(window));
        const auto beforeRefresh=api.generation();session.requestRefresh();
        QTRY_VERIFY(session.ready()&&api.generation()>beforeRefresh);QTRY_VERIFY(!composerEmojiContent(window));
        QVERIFY(controller.ownStatusContextKey()!=context);
        for(const auto &request:fixture.server.requests)
            QVERIFY(request.method=="GET"||request.target.path().contains("/iam/"));
        QCOMPARE(fixture.posts,0);QCOMPARE(fixture.memberPosts,0);
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const bool closed=closeIncubatedTestWindow(engine,window);if(closed)close.dismiss();QVERIFY(closed);
    }

    void canonicalUsersHttpNameAppearsInTheActualMainAuthorLabel()
    {
        const QString peer = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const QJsonArray directory{QJsonObject{{"uuid", peer}, {"project_id", projectId},
            {"username", "alice"}, {"first_name", " Alice "}, {"last_name", " Synthetic "},
            {"source", "iam"}, {"status", "offline"}, {"avatar", QJsonValue::Null},
            {"email", "alice@example.invalid"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}}};
        auto served = message("Synthetic ordinary author witness");
        served["author_uuid"] = peer; served["is_own"] = false;
        int usersGets = 0, historyGets = 0, mutations = 0;
        MockServer server;
        server.handler = [&, directory, served](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/") return {QJsonArray{windowChannel(streamId, "Synthetic ordinary channel")}};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId) return {windowChannel(streamId, "Synthetic ordinary channel")};
            if (path.endsWith("/users/")) { ++usersGets; return {directory}; }
            if (path == "/api/workspace/v1/messenger/stream_topics/") return {QJsonArray{windowChannelTopic(topicId, streamId, "Synthetic topic")}};
            if (path == "/api/workspace/v1/messenger/stream_topics/" + topicId) return {windowChannelTopic(topicId, streamId, "Synthetic topic")};
            if (path.endsWith("/messages/")) {
                if (request.method == "GET") ++historyGets; else ++mutations;
                return {QJsonArray{served}};
            }
            return sessionResponse(request);
        };
        QTemporaryDir directoryPath; QVERIFY(directoryPath.isValid());
        NativePreferences preferences(directoryPath.filePath("canonical-author.ini")); preferences.setLocale("en");
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([guard = QPointer<QQuickWindow>(window), &engine] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Author witness window did not settle and release focus before teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        controller.login(server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1); controller.selectTopic(topicId);
        QTRY_VERIFY(!controller.busy()); QTRY_COMPARE(controller.messages().size(), 1);
        QTRY_COMPARE(usersGets, 1); QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(historyGets > 0); QCOMPARE(mutations, 0);
        QSignalSpy frame(window, &QQuickWindow::frameSwapped); QVERIFY(frame.isValid()); window->update(); QTRY_VERIFY(!frame.isEmpty());
        QTRY_VERIFY(visibleItem(window->contentItem(), "message-" + messageId));
        QTRY_VERIFY(visibleItem(window->contentItem(), "message-author-" + messageId));
        auto *label = visibleItem(window->contentItem(), "message-author-" + messageId); QVERIFY(label);
        QCOMPARE(label->property("textFormat").toInt(), 0); // Text.PlainText.
        // The actual HTTP canonical DTO has no legacy name/display_name fields.
        // Main must show the same Source display name as its existing directory projection.
        QCOMPARE(controller.messages().first().toMap().value("authorId").toString(), peer);
        QCOMPARE(label->property("text").toString(), QString("Alice Synthetic"));
        const bool closed = closeIncubatedTestWindow(engine, window); if (closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void visibleGenericAuthorUsesSourceUnknownFallback_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"missing-user", "known-name", "null-optional-names"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void visibleGenericAuthorUsesSourceUnknownFallback()
    {
        QFETCH(QString, scenario);
        const QString peer = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const bool named = scenario == "known-name";
        const bool usernameOnly = scenario == "null-optional-names";
        const QString preferred = "<b>Known peer</b>";
        const QString username = "known-peer";
        QJsonArray directory;
        if (named || usernameOnly) directory.append(QJsonObject{{"uuid", peer}, {"username", username},
            {"source", "iam"}, {"status", "offline"}, {"last_ping_at", "2026-01-02T10:00:00Z"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"},
            {"name", named ? QJsonValue(preferred) : QJsonValue::Null}, {"display_name", QJsonValue::Null},
            {"first_name", named ? QJsonValue("<b>Known") : QJsonValue::Null},
            {"last_name", named ? QJsonValue("peer</b>") : QJsonValue::Null}});
        auto served = message("Synthetic ordinary author witness");
        served["author_uuid"] = peer; served["is_own"] = false;
        int usersGets = 0, historyGets = 0, mutations = 0;
        MockServer server;
        server.handler = [&, directory, served](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/streams/") return {QJsonArray{windowChannel(streamId, "Synthetic ordinary channel")}};
            if (path == "/api/workspace/v1/messenger/streams/" + streamId) return {windowChannel(streamId, "Synthetic ordinary channel")};
            if (path.endsWith("/users/")) { ++usersGets; return {directory}; }
            if (path == "/api/workspace/v1/messenger/stream_topics/") return {QJsonArray{windowChannelTopic(topicId, streamId, "Synthetic topic")}};
            if (path == "/api/workspace/v1/messenger/stream_topics/" + topicId) return {windowChannelTopic(topicId, streamId, "Synthetic topic")};
            if (path.endsWith("/messages/")) {
                if (request.method == "GET") ++historyGets; else ++mutations;
                return {QJsonArray{served}};
            }
            return sessionResponse(request);
        };
        QTemporaryDir directoryPath; QVERIFY(directoryPath.isValid());
        NativePreferences preferences(directoryPath.filePath("author-fallback.ini")); preferences.setLocale("en");
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([guard = QPointer<QQuickWindow>(window), &engine] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Author witness window did not settle and release focus before teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        controller.login(server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1); controller.selectTopic(topicId);
        QTRY_VERIFY(!controller.busy()); QTRY_COMPARE(controller.messages().size(), 1);
        QTRY_COMPARE(usersGets, 1); QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(historyGets > 0); QCOMPARE(mutations, 0);
        QSignalSpy frame(window, &QQuickWindow::frameSwapped); QVERIFY(frame.isValid()); window->update(); QTRY_VERIFY(!frame.isEmpty());
        QTRY_VERIFY(visibleItem(window->contentItem(), "message-" + messageId));
        QTRY_VERIFY(visibleItem(window->contentItem(), "message-author-" + messageId));
        auto *label = visibleItem(window->contentItem(), "message-author-" + messageId); QVERIFY(label);
        QCOMPARE(label->property("textFormat").toInt(), 0); // Text.PlainText, including the named markup-shaped control.
        const auto expected = named ? preferred : usernameOnly ? username : "#" + peer.left(8);
        QCOMPARE(label->property("text").toString(), expected);
        const bool closed = closeIncubatedTestWindow(engine, window); if (closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
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
            auto *field=item(window->contentItem(),"serverInput"); QVERIFY(field && field->isVisible());
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
            auto *field=item(window->contentItem(),"serverInput"); QVERIFY(field && field->isVisible());
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

    void selectedDeletedTopicKeepsAnExplicitMainRouteUntilCanonicalOpening_data()
    {
        QTest::addColumn<bool>("chooseNewer");
        QTest::newRow("selected-deletion-opens-owned-canonical-topic") << false;
        QTest::newRow("real-newer-sidebar-click-wins-over-held-alias") << true;
    }

    void selectedDeletedTopicKeepsAnExplicitMainRouteUntilCanonicalOpening()
    {
        QFETCH(bool, chooseNewer);
        // Deferred transport and observer captures outlive Fixture/Controller.
        std::function<void()> releaseLookup;
        bool retired = false, observeAggregate = false, sawAggregate = false;
        WindowTopicLinkFixture fixture("cross-canonical");
        const auto canonical = windowChannelTopic(linkCanonicalId, linkDestinationId, "Linked destination");
        const auto oldBody = message("Retired topic body must disappear", messageId, topicId);
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&, fallback, canonical, oldBody](const Request &request) -> Response {
            const auto path = request.target.path();
            if (retired && request.method == "GET" && path == "/api/workspace/v1/messenger/stream_topics/" + topicId) {
                ++fixture.lookupGets;
                Response response{canonical};
                response.defer = [&releaseLookup](std::function<void()> send) { releaseLookup = std::move(send); };
                return response;
            }
            auto response = fallback(request); // Keep existing real HTTP accounting.
            if (request.method != "GET") return response;
            if (path == "/api/workspace/v1/messenger/streams/") {
                auto rows = response.data.toArray();
                rows.append(windowChannel(linkDestinationId, "Destination channel"));
                response.data = rows; // B is already an owned known channel.
            }
            const QUrlQuery query(request.target);
            if (path == "/api/workspace/v1/messenger/stream_topics/" && retired
                && query.queryItemValue("stream_uuid") == streamId)
                response.data = QJsonArray{windowChannelTopic(secondTopicId, streamId, "Planning")};
            if (path == "/api/workspace/v1/messenger/messages/"
                && query.queryItemValue("stream_uuid") == streamId) {
                if (query.queryItemValue("topic_uuid") == topicId) response.data = QJsonArray{oldBody};
                if (query.queryItemValue("topic_uuid") == secondTopicId)
                    response.data = QJsonArray{message("Latest Planning selection stays visible", returnedId, secondTopicId)};
            }
            return response;
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("selected-deletion.ini"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        FileClient files(&api); controller.useFiles(&files); // Non-vacuous attachment baseline.
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto closeBeforeEngine = qScopeGuard([guard = QPointer<QQuickWindow>(window), &engine] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Canonical-route Main window did not settle and release focus before teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE_WITH_TIMEOUT(session.projects().size(), 1, 2000); session.selectProject(projectId);
        QTRY_VERIFY_WITH_TIMEOUT(controller.channelMembersVerified(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.streams().size(), 2, 2000);
        // Same normal window-fixture mode as the existing canonical-link test:
        // polling is stopped after bootstrap; no realtime transport is claimed.
        controller.setRealtimeMode(true);
        preferences.setOwner(controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");

        const auto renderedFrame = [&] {
            QSignalSpy frame(window, &QQuickWindow::frameSwapped);
            if (!frame.isValid()) return false;
            window->update();
            return QTest::qWaitFor([&] { return !frame.isEmpty(); }, 2000);
        };
        // Resolve the real current item and map only after the caller's frame.
        // No global helper, artificial offset, forced layout or input delay.
        const auto pointerClick = [&](const QString &name) {
            QPointer<QQuickItem> target = visibleItem(window->contentItem(), name);
            if (!target || target->window() != window || target->width() <= 0 || target->height() <= 0) return false;
            const QRectF bounds(target->mapToScene(QPointF{}), target->size());
            if (!QRectF(0, 0, window->width(), window->height()).contains(bounds)) return false;
            QTest::mouseClick(window, Qt::LeftButton, {}, bounds.center().toPoint());
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(visibleItem(window->contentItem(), "stream-" + streamId), 2000);
        QVERIFY(renderedFrame());
        {
            QSignalSpy chosen(visibleItem(window->contentItem(), "stream-" + streamId), SIGNAL(clicked())); QVERIFY(chosen.isValid());
            QVERIFY(pointerClick("stream-" + streamId)); QTRY_COMPARE_WITH_TIMEOUT(chosen.size(), 1, 2000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller.topics().size(), 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(visibleItem(window->contentItem(), "topic-" + topicId), 2000);
        QVERIFY(renderedFrame());
        {
            QSignalSpy chosen(visibleItem(window->contentItem(), "topic-" + topicId), SIGNAL(clicked())); QVERIFY(chosen.isValid());
            QVERIFY(pointerClick("topic-" + topicId)); QTRY_COMPARE_WITH_TIMEOUT(chosen.size(), 1, 2000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && controller.messages().size() == 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(visibleItem(window->contentItem(), "message-content-" + messageId), 2000);
        QVERIFY(visibleItem(window->contentItem(), "message-content-" + messageId)->property("text").toString()
            .contains("Retired topic body must disappear")); // Actual RichText also contains inline time markup.
        QPointer<QQuickItem> header = item(window->contentItem(), "conversationHeader");
        QPointer<QQuickItem> list = item(window->contentItem(), "messageList");
        QPointer<QQuickItem> prompt = item(window->contentItem(), "streamTopicPrompt");
        QPointer<QQuickItem> shell = item(window->contentItem(), "messageComposerShell");
        QPointer<QQuickItem> composer = item(window->contentItem(), "messageComposer");
        QPointer<QQuickItem> send = item(window->contentItem(), "sendButton");
        QPointer<QQuickItem> attach = item(window->contentItem(), "attachButton");
        QVERIFY(header && list && prompt && shell && composer && send && attach);
        QVERIFY(hasVisibleText(header, "General Topic")); QVERIFY(hasVisibleText(header, " · Product team"));
        QVERIFY(shell->isVisible() && composer->isVisible());
        QVERIFY(!prompt->isVisible()); QVERIFY(!window->property("aggregateConversation").toBool());
        QVERIFY(renderedFrame()); QVERIFY(pointerClick("messageComposer"));
        QTRY_VERIFY_WITH_TIMEOUT(composer->hasActiveFocus(), 2000);
        const QByteArray originalDraft("original topic draft is still local");
        for (const auto key : originalDraft) QTest::keyClick(window, key);
        QTRY_COMPARE_WITH_TIMEOUT(controller.draft(), QString::fromLatin1(originalDraft), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled() && attach->isEnabled(), 2000);
        QVERIFY(controller.canSend() && controller.canAttach());
        QTRY_VERIFY_WITH_TIMEOUT(api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(fixture.lookupGets, 0);
        QVERIFY(renderedFrame());

        const auto promptChange = connect(prompt.data(), &QQuickItem::visibleChanged, &controller, [&] {
            if (observeAggregate && prompt && prompt->isVisible()) sawAggregate = true;
        });
        const auto observeRoute = [&] {
            if (observeAggregate && controller.selectedTopic().isEmpty()) sawAggregate = true;
        };
        const auto topicChange = connect(&controller, &NativeController::topicsChanged, &controller, observeRoute);
        const auto messageChange = connect(&controller, &NativeController::messagesChanged, &controller, observeRoute);
        const auto stateChange = connect(&controller, &NativeController::stateChanged, &controller, observeRoute);
        const auto disconnectObservers = qScopeGuard([&] {
            disconnect(promptChange); disconnect(topicChange); disconnect(messageChange); disconnect(stateChange);
        });
        fixture.record = true; retired = observeAggregate = true;
        QVERIFY(controller.applyRealtimeEvent({{"project_id", projectId}, {"user_uuid", userId},
            {"uuid", "99000000-0000-4000-8000-000000000061"},
            {"payload", QJsonObject{{"kind", "topic.deleted"}, {"uuid", topicId}, {"stream_uuid", streamId}}}}));
        // Positive real HTTP admission, not a direct openTopic invocation.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.lookupGets, 1, 2000); QVERIFY(releaseLookup);
        QVERIFY(controller.topicResolving());
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.draft(), QString::fromLatin1(originalDraft));
        QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 0, 2000);
        QVERIFY(renderedFrame());
        QVERIFY(!visibleItem(window->contentItem(), "message-content-" + messageId));
        QVERIFY(!visibleItem(window->contentItem(), "topic-" + topicId));
        QVERIFY(!prompt->isVisible()); QVERIFY(!window->property("aggregateConversation").toBool()); QVERIFY(!sawAggregate);
        QVERIFY(shell->isVisible() && composer->isVisible());
        QCOMPARE(composer->property("text").toString(), QString::fromLatin1(originalDraft));
        QVERIFY(!send->isEnabled() && !attach->isEnabled()); QVERIFY(!controller.canSend() && !controller.canAttach());
        {
            QSignalSpy sent(send.data(), SIGNAL(clicked())); QSignalSpy attached(attach.data(), SIGNAL(clicked()));
            QVERIFY(sent.isValid() && attached.isValid());
            QVERIFY(pointerClick("sendButton")); QVERIFY(pointerClick("attachButton"));
            QCOMPARE(sent.size(), 0); QCOMPARE(attached.size(), 0);
        }
        QCOMPARE(fixture.routeHistoryGets, 0); QCOMPARE(fixture.routeTopicGets, 0);
        QCOMPARE(fixture.aggregateGets, 0); QCOMPARE(fixture.mutations, 0);

        if (chooseNewer) {
            QTRY_VERIFY_WITH_TIMEOUT(visibleItem(window->contentItem(), "topic-" + secondTopicId), 2000);
            QVERIFY(renderedFrame());
            QSignalSpy chosen(visibleItem(window->contentItem(), "topic-" + secondTopicId), SIGNAL(clicked())); QVERIFY(chosen.isValid());
            QVERIFY(pointerClick("topic-" + secondTopicId)); QTRY_COMPARE_WITH_TIMEOUT(chosen.size(), 1, 2000);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving() && !controller.busy(), 2000);
        }
        // Deliver the identical held canonical result once, also safe after the
        // newer real click has cancelled its socket through existing TopicOpening.
        auto deliver = std::move(releaseLookup); releaseLookup = {}; QVERIFY(deliver); deliver();
        const auto expectedStream = chooseNewer ? streamId : linkDestinationId;
        const auto expectedTopic = chooseNewer ? secondTopicId : linkCanonicalId;
        const auto expectedTitle = chooseNewer ? QString("Planning") : QString("Linked destination");
        const auto expectedBody = chooseNewer ? QString("Latest Planning selection stays visible") : QString("The canonical topic is open");
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), expectedStream, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), expectedTopic, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving() && !controller.busy(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(visibleItem(window->contentItem(), "message-content-" + returnedId), 2000);
        QVERIFY(renderedFrame());
        QVERIFY(hasVisibleText(header, expectedTitle));
        QVERIFY(hasVisibleText(header, chooseNewer ? QString(" · Product team") : QString(" · Destination channel")));
        QVERIFY(visibleItem(window->contentItem(), "message-content-" + returnedId)->property("text").toString().contains(expectedBody));
        QCOMPARE(controller.messages().size(), 1);
        QCOMPARE(controller.messages().first().toMap().value("id").toString(), returnedId);
        QCOMPARE(controller.messages().first().toMap().value("content").toString(), expectedBody);
        QVERIFY(!visibleItem(window->contentItem(), "message-content-" + messageId));
        auto *selected = visibleItem(window->contentItem(), "topic-" + expectedTopic); QVERIFY(selected);
        QCOMPARE(QQmlProperty::read(selected, "background.color").value<QColor>(),
            QColor(window->property("colors").toMap().value("card-bg-active").toString()));
        QVERIFY(!prompt->isVisible()); QVERIFY(!window->property("aggregateConversation").toBool()); QVERIFY(!sawAggregate);
        QVERIFY(shell->isVisible() && composer->isVisible() && composer->isEnabled());
        QVERIFY(pointerClick("messageComposer")); QTRY_VERIFY_WITH_TIMEOUT(composer->hasActiveFocus(), 2000);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        const QByteArray freshDraft("fresh canonical route draft");
        for (const auto key : freshDraft) QTest::keyClick(window, key);
        QTRY_COMPARE_WITH_TIMEOUT(controller.draft(), QString::fromLatin1(freshDraft), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled() && attach->isEnabled(), 2000);
        QVERIFY(controller.canSend() && controller.canAttach()); QVERIFY(controller.error().isEmpty());
        QCOMPARE(fixture.lookupGets, 1); QCOMPARE(fixture.detailGets, 0);
        QCOMPARE(fixture.routeTopicGets, 1); QCOMPARE(fixture.routeHistoryGets, 1);
        QCOMPARE(fixture.aggregateGets, 0); QCOMPARE(fixture.mutations, 0);
        QVERIFY2(fixture.unexpected.isEmpty(), qPrintable(fixture.unexpected.join(QStringLiteral("; "))));
        QCOMPARE(fixture.historyTargets.size(), 1);
        const QUrlQuery history(fixture.historyTargets.first());
        QCOMPARE(history.queryItemValue("stream_uuid"), expectedStream);
        QCOMPARE(history.queryItemValue("topic_uuid"), expectedTopic);
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
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
        QJsonArray inputTrace;
        const auto trace = [&](const QString &phase) {
            auto *focus = window->activeFocusItem();
            const auto at = search->mapToScene(QPointF(search->width() / 2, search->height() / 2));
            inputTrace.append(QJsonObject{{"phase",phase},{"text",search->property("text").toString()},
                {"activeFocus",search->hasActiveFocus()},{"focusItem",focus?focus->objectName():QString{}},
                {"visible",search->isVisible()},{"enabled",search->isEnabled()},{"windowActive",window->isActive()},
                {"count",list->property("count").toInt()},{"tab",window->findChild<QObject *>("createChatTabs")->property("currentIndex").toInt()},
                {"frames",rendered.size()},{"x",at.x()},{"y",at.y()},{"selectedText",search->property("selectedText").toString()}});
        };
        const auto traceOnExit = qScopeGuard([&] { qInfo().noquote() << "Synthetic archive input trace" << QJsonDocument(inputTrace).toJson(QJsonDocument::Compact); });
        click(window,search); trace("initial-click");
        const auto capture=[&](const QString &state) { if(scenario!="virtual")captureArchiveWindow(window,scenario,state); };
        capture("normal");
        if(scenario=="virtual"){QVERIFY(itemCountWithPrefix(window->contentItem(),"archive-row-")<30);QCOMPARE(controller.archivedChats().size(),505);}
        for(const auto key:QByteArray("  ALICE  "))QTest::keyClick(window,key);QTRY_COMPARE(list->property("count").toInt(),1);QVERIFY(visibleItem("archive-open-"+archiveWindowId));
        click(window,visibleItem("createChatTab-0"));click(window,visibleItem("createChatTab-3"));QCOMPARE(search->property("text").toString(),QString("  ALICE  "));
        trace("returned-to-archive"); click(window,search); trace("replacement-click");
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier); trace("select-all");
        for(const auto key:QByteArray("zzzz-no-synthetic-chat"))QTest::keyClick(window,key);
        trace("typed-replacement"); QTRY_COMPARE(list->property("count").toInt(),0);QVERIFY(visibleItem("archiveEmptyStatus"));capture("empty");
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
        fixture.applyRestore=false;fixture.restoreStatus=403;fixture.holdRestores=true;
        const auto release=qScopeGuard([&]{for(const auto &id:fixture.pendingRestores.keys())fixture.finishRestore(id);});
        ApiClient api(nullptr,true);Session session(&api);NativeController controller(&api,&session);QQmlApplicationEngine engine;loadNativeUi(engine,&controller,&preferences);
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));QVERIFY(window);QVERIFY(QTest::qWaitForWindowExposed(window));session.login(fixture.server.origin(),"synthetic","synthetic");QTRY_COMPARE(session.projects().size(),1);session.selectProject(projectId);QTRY_VERIFY(controller.channelMembersVerified());openArchive(window);
        click(window,visibleItem("archive-restore-"+archiveWindowId));
        QTRY_COMPARE(fixture.restorePosts,1);
        QSignalSpy pendingFrame(window,&QQuickWindow::frameSwapped);window->update();QTRY_VERIFY(!pendingFrame.isEmpty());
        // The first pending state replaces delegates. Resolve the next row only
        // after that frame; both HTTP requests still overlap and complete out of order.
        QVERIFY(controller.canUnarchiveChat(archiveRowId(2)));
        click(window,visibleItem("archive-restore-"+archiveRowId(2)));QTRY_COMPARE(fixture.restorePosts,2);
        QTRY_COMPARE(fixture.pendingRestores.size(),2);
        fixture.finishRestore(archiveRowId(2));QTRY_COMPARE(window->property("archiveErrorTarget").toString(),archiveRowId(2));
        fixture.finishRestore(archiveWindowId);QTRY_COMPARE(window->property("archiveErrorTarget").toString(),archiveWindowId);
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
        // #16: single-line font-derived tab height drifts by 1/64 px across host font stacks;
        // the breakpoint classes this oracle separates stay >= 1.5 px apart.
        QTRY_VERIFY(qAbs(visibleItem("createChatTabs")->height()-height)<=0.5);
        QCOMPARE(QQmlProperty::read(visibleItem("createChatTab-" + QString::number(tab)), "contentItem.font.pixelSize").toInt(), width < 640 ? 12 : 14);
        // #16: composed form content height drifts by up to 1 px across host font stacks.
        if (tab == 1) QVERIFY(qAbs(visibleItem("channelFormBody")->property("contentHeight").toReal()-qreal(600.53125))<=2.0);
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
        if (scenario == "archived" || scenario == "403") {
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
        if (scenario == "encrypted") {
            QTRY_VERIFY(visibleItem("encryptedReadStatus"));
            QVERIFY(!visibleItem("streamTopicPrompt"));
        } else {
            QTRY_VERIFY(visibleItem("streamTopicPrompt"));
        }
        QVERIFY(!controller.canSend() && !controller.canAttach());
        QTRY_VERIFY(visibleItem("directConversationName"));
        QCOMPARE(visibleItem("directConversationName")->property("text").toString(), QString("Alice"));
        QTRY_COMPARE(visibleItem("conversationHeader")->height(), qreal(56));
        QCOMPARE(item(window->contentItem(), "stream-" + windowDirectId())->property("enabled").toBool(), true);
        QCOMPARE(fixture.memberPosts, 0);
        QCOMPARE(fixture.posts, scenario == "existing" || scenario == "encrypted" ? 0 : scenario == "not-applied503" ? 2 : 1);
        if (!fixture.directBodies.isEmpty())
            QCOMPARE(fixture.directBodies.first(), QJsonObject({{"name", "Direct"}, {"description", "Private workspace"},
                {"source_name", "native"}, {"direct_user_uuid", channelPeer}}));
        click(window, item(window->contentItem(), "topic-" + channelDefault));
        QTRY_COMPARE(controller.selectedTopic(), channelDefault);
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(visibleItem("directConversationName")->property("text").toString(), QString("Alice"));
        controller.setDraft("A real selected topic enables the composer");
        QCOMPARE(controller.canSend(), scenario != "encrypted"); // No Vault/key: the encrypted route stays locked.
        window->close();
    }

    void existingEncryptedDirectFromNewChatOpensWithoutChangingThePair_data()
    {
        QTest::addColumn<bool>("currentKeyKnown");
        QTest::newRow("known-current") << true;
        QTest::newRow("null-current") << false;
    }
    void existingEncryptedDirectFromNewChatOpensWithoutChangingThePair()
    {
        QFETCH(bool, currentKeyKnown);
        QTest::failOnWarning(QRegularExpression(".*"));
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("existing-encrypted-direct.ini"));
        WindowDirectFixture fixture; fixture.seed();
        fixture.created["encryption"] = true;
        fixture.created["current_encryption_key_uuid"] = currentKeyKnown
            ? QJsonValue(returnedId) : QJsonValue(QJsonValue::Null);
        const auto originalPair = fixture.created;
        const auto originalTopic = fixture.defaultTopic;
        int acknowledgedPairGets = 0, unexpectedWrites = 0;
        QJsonObject servedPair;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (request.method != "GET" && request.target.path().startsWith("/api/workspace/v1/messenger/"))
                ++unexpectedWrites;
            auto response = fallback(request);
            if (request.method == "GET"
                && request.target.path() == "/api/workspace/v1/messenger/streams/" + windowDirectId()) {
                ++acknowledgedPairGets;
                servedPair = response.data.toObject();
            }
            return response;
        };
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QSignalSpy opened(&controller, &NativeController::directChatOpened); QVERIFY(opened.isValid());
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Existing encrypted Direct window did not settle before teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(controller.directChatDirectoryVerified());
        QTRY_VERIFY(controller.canStartDirectChatWith(channelPeer));
        auto *newChat = item(window->contentItem(), "createChatButton"); QVERIFY(newChat && newChat->isEnabled());
        click(window, newChat);
        QTRY_VERIFY(visibleItem(window->contentItem(), "directChatSearch"));
        click(window, visibleItem(window->contentItem(), "directChatSearch"));
        for (const auto key : QByteArray("alice")) QTest::keyClick(window, key);
        QTRY_COMPARE(visibleItem(window->contentItem(), "directChatSearch")->property("text").toString(), QString("alice"));
        QTRY_VERIFY(visibleItem(window->contentItem(), "direct-user-" + channelPeer)
            && visibleItem(window->contentItem(), "direct-user-" + channelPeer)->isEnabled());
        auto *peer = visibleItem(window->contentItem(), "direct-user-" + channelPeer); QVERIFY(peer);
        const auto point = peer->mapToScene(QPointF(peer->width() / 2, peer->height() / 2));
        QVERIFY(QRectF(QPointF{}, QSizeF(window->size())).contains(point));
        click(window, peer);
        QTRY_COMPARE(acknowledgedPairGets, 1); // Actual server acknowledgement, not merely an allocated QNetworkReply.
        QTRY_VERIFY(!controller.directChatBusy());
        QCOMPARE(servedPair, originalPair);
        QCOMPARE(servedPair.value("uuid").toString(), windowDirectId());
        QCOMPARE(servedPair.value("project_id").toString(), projectId);
        QCOMPARE(servedPair.value("user_uuid").toString(), userId);
        QCOMPARE(servedPair.value("direct_user_uuid").toString(), channelPeer);
        QVERIFY(servedPair.value("encryption").toBool());
        QVERIFY(!servedPair.value("is_archived").toBool());
        QCOMPARE(fixture.posts, 0); QCOMPARE(fixture.memberPosts, 0); QCOMPARE(unexpectedWrites, 0);
        // First intended product oracle: the successful owned existing pair opens.
        // No crypto/Vault/read/send assertion is used to manufacture this route RED.
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(controller.directChatPhase(), QString("ready"));
        QCOMPARE(controller.selectedStream(), windowDirectId());
        QCOMPARE(controller.selectedTopic(), QString{}); // Source navigates the stream, not an invented selected topic.
        QCOMPARE(controller.conversationKind(), QString("stream"));
        QVERIFY(controller.isDirectConversation());
        QCOMPARE(controller.directPeer().value("id").toString(), channelPeer);
        QTRY_VERIFY(!visibleItem(window->contentItem(), "directChatSearch"));
        QTRY_VERIFY(visibleItem(window->contentItem(), "directConversationName"));
        QCOMPARE(fixture.created, originalPair); QCOMPARE(fixture.defaultTopic, originalTopic);
        QVERIFY(fixture.topicGets >= 1); // Existing verified-default/topics publication must actually occur.
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach()); // No Vault/key is fabricated here.
        QCOMPARE(fixture.posts, 0); QCOMPARE(fixture.memberPosts, 0); QCOMPARE(unexpectedWrites, 0);
        const bool closed = closeIncubatedTestWindow(engine, window);
        if (closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
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
        QElapsedTimer clock;
        clock.start();
        QJsonArray perfPhases;
        qint64 perfTotalMs = 0;
        const auto perfPhase = [&](const QString &name, qint64 ms) {
            perfTotalMs += ms;
            perfPhases.append(QJsonObject{{"name", name}, {"ms", ms}});
        };
        QTRY_COMPARE(session.projects().size(), 1);
        perfPhase("sign-in", clock.restart());
        click(window, item(content, "project-" + projectId));
        QTRY_COMPARE(controller.streams().size(), 1);
        perfPhase("project-open", clock.restart());
        click(window, item(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 2);
        perfPhase("stream-open-1-cold", clock.restart());
        const QStringList topicPhaseNames{QStringLiteral("topic-select-1-cold"),
            QStringLiteral("topic-select-2-cold"), QStringLiteral("topic-select-3-warm")};
        int topicPhaseIndex = 0;
        for (const auto &id : {topicId, secondTopicId, topicId}) {
            click(window, item(content, "topic-" + id));
            QTRY_VERIFY(!controller.busy());
            QTRY_VERIFY(item(content, "message-content-" + (id == topicId ? messageId : returnedId)));
            QVERIFY(!controller.cacheStale());
            perfPhase(topicPhaseNames[topicPhaseIndex++], clock.restart());
        }
        QCOMPARE(historyGets, 2);
        QCOMPARE(aggregateGets, 1);
        QCOMPARE(topicGets, 1);
        click(window, item(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 2);
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.messages().size(), 2);
        perfPhase("stream-open-2-warm", clock.restart());
        click(window, item(content, "topic-" + secondTopicId));
        QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(item(content, "message-content-" + returnedId));
        perfPhase("topic-select-4-warm", clock.restart());
        QCOMPARE(historyGets, 2);
        QCOMPARE(aggregateGets, 1);
        QCOMPARE(topicGets, 1);
        QTRY_VERIFY(cache.stats().hits >= 3);
        qInfo("Warm navigation: 4 topic selections, topic-history GETs=%d; 2 stream selections, aggregate-history GETs=%d, topic-list GETs=%d; cache hits=%llu",
            historyGets, aggregateGets, topicGets, static_cast<unsigned long long>(cache.stats().hits));
        const auto perfMetricsPath = qEnvironmentVariable("CASSI_NATIVE_TEST_PERF_METRICS");
        if (!perfMetricsPath.isEmpty()) {
            QFile metrics(perfMetricsPath);
            QVERIFY(metrics.open(QIODevice::WriteOnly));
            const auto data = QJsonDocument(QJsonObject{{"version", 1},
                {"case", QStringLiteral("warmNavigationUsesVerifiedSavedPagesWithoutExtraHttpRequests")}, {"synthetic", true},
                {"requests", QJsonObject{{"topic-history", historyGets}, {"aggregate-history", aggregateGets}, {"topic-list", topicGets}}},
                {"cacheHits", static_cast<double>(cache.stats().hits)},
                {"phases", perfPhases},
                {"totalMs", static_cast<double>(perfTotalMs)}}).toJson();
            QCOMPARE(metrics.write(data), data.size());
        }
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
        // #16: the composed form height drifts by up to 1 px across host font stacks.
        QVERIFY(qAbs(dialog->property("height").toReal()-qreal(749.53125))<=2.0);
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
        // #16: drifts by 1/64 px with host font line heights.
        QVERIFY(qAbs(body->height()-qreal(375.984375))<=0.5);
        // #16: drifts with host font metrics like the composed form above.
        QVERIFY(qAbs(body->property("contentHeight").toReal()-qreal(600.53125))<=2.0);
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

    // docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md (issue #2): the measured preview line and the
    // time column (workspace_ui 61dacef widgets/sidebar/sidebar-message-preview.ui.tsx and
    // sidebar-chat-row-meta.ui.tsx) render inside the existing 60 px stream and 56 px topic
    // rows. Colors are the blue-cold light palette of qml/NativeTheme.qml: sidebar-sender
    // #7087ff, text-secondary #787878, text-muted #989898.
    void sidebarRowsRenderPreviewAndTime()
    {
        WindowChannelFixture fixture;
        fixture.sourceScene = true;
        const auto fallback = fixture.server.handler;
        // The newest source-scene message is the last-message pointer to attach; the stream
        // list/detail DTOs and the default topic DTO must all carry it or the projection
        // drops the pointer before the rows are built.
        const QString lastMessage = QStringLiteral("81000000-0000-4000-8000-000000000007");
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            auto response = fallback(request);
            if (request.method != "GET") return response;
            const bool streamRoute = path.endsWith("/streams/")
                || path.endsWith("/streams/" + streamId)
                || path.endsWith("/stream_topics/") || path.endsWith("/stream_topics/" + topicId);
            if (!streamRoute) return response;
            if (response.data.isArray()) {
                QJsonArray patched;
                for (const auto &value : response.data.toArray()) {
                    auto row = value.toObject();
                    if (row.value("uuid").toString() == streamId || row.value("uuid").toString() == topicId)
                        row["last_message_uuid"] = lastMessage;
                    patched.append(row);
                }
                response.data = patched;
            } else if (response.data.isObject()
                && response.data.toObject().value("uuid").toString() == streamId) {
                auto row = response.data.toObject();
                row["last_message_uuid"] = lastMessage;
                response.data = row;
            }
            return response;
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("sidebar-previews.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        ApiClient api(nullptr,true); Session session(&api);
        NativeController controller(&api,&session);
        QQmlApplicationEngine engine; loadNativeUi(engine,&controller,&preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine,guard=QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine,guard.data()))
                QTest::qFail("Sidebar preview window did not settle before engine teardown",__FILE__,__LINE__);
        });
        window->resize(1440,900); QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(),"synthetic","synthetic");
        QTRY_COMPARE(session.projects().size(),1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(),1); QTRY_VERIFY(controller.channelMembersVerified());
        QTRY_VERIFY(visibleItem("stream-"+streamId));
        click(window,visibleItem("stream-"+streamId));
        QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(controller.topics().size() >= 3);
        // The source-scene author is the Alice user of the fixture catalog (first_name only).
        const QString previewText = QStringLiteral("A message with missing topic metadata has no invented label.");
        // The preview exists only after the selected channel's history has been hydrated.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto *row = visibleItem("streamPreviewText-" + streamId);
            return row && row->property("text").toString() == previewText;
        })(), 4000);
        QTRY_VERIFY(visibleItem("streamPreviewSender-"+streamId));
        QCOMPARE(visibleItem("streamPreviewSender-"+streamId)->property("text").toString(),QStringLiteral("Alice: "));
        QCOMPARE(visibleItem("streamPreviewText-"+streamId)->property("text").toString(),previewText);
        QCOMPARE(visibleItem("streamPreviewSender-"+streamId)->property("color").value<QColor>(),QColor("#7087ff"));
        QCOMPARE(visibleItem("streamPreviewText-"+streamId)->property("color").value<QColor>(),QColor("#787878"));
        QCOMPARE(visibleItem("streamPreviewText-"+streamId)->property("font").value<QFont>().pixelSize(),12);
        // The row time is the last-message instant (2026-01-02T10:06:00Z, the seventh source
        // message) under formatMessageTimeRelative, muted token, bottom of the meta column.
        const auto created = QDateTime::fromString("2026-01-02T10:06:00.000Z",Qt::ISODateWithMs);
        auto *streamTime = visibleItem("streamTime-"+streamId); QVERIFY(streamTime);
        QCOMPARE(streamTime->property("text").toString(),NativeController::formatRowTime(
            created,QLocale("en"),QDate::currentDate(),QStringLiteral("Yesterday")));
        QCOMPARE(streamTime->property("color").value<QColor>(),QColor("#989898"));
        QCOMPARE(visibleItem("stream-"+streamId)->height(),60); // the measured row box is unchanged
        // The default topic row carries the same message; Planning has no last message and
        // still keeps the full 56 px row with its empty 20 px preview line mounted.
        QVERIFY(visibleItem("topic-"+topicId));
        QCOMPARE(visibleItem("topic-"+topicId)->height(),56);
        QCOMPARE(visibleItem("topicPreviewText-"+topicId)->property("text").toString(),previewText);
        QCOMPARE(visibleItem("topicTime-"+topicId)->property("text").toString(),
            streamTime->property("text").toString());
        QVERIFY(visibleItem("topic-"+secondTopicId));
        QCOMPARE(visibleItem("topic-"+secondTopicId)->height(),56);
        auto *planningLine = visibleItem("topicPreviewLine-"+secondTopicId); QVERIFY(planningLine);
        QCOMPARE(planningLine->height(),20); // an unresolved row keeps the min-h-5 line box
        QCOMPARE(visibleItem("topicPreviewSender-"+secondTopicId)->property("text").toString(),QString());
        QCOMPARE(visibleItem("topicPreviewText-"+secondTopicId)->property("text").toString(),QString());
        QVERIFY(!visibleItem("topicTime-"+secondTopicId));
        const bool closed = closeIncubatedTestWindow(engine,window); if (closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
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

    void captureAggregateScene(QQuickWindow *window, const QString &scenario, const QJsonArray &bubbles = {})
    {
        const auto captureDirectory = qEnvironmentVariable("CASSI_NATIVE_TEST_AGGREGATE_CAPTURE_DIR");
        if (captureDirectory.isEmpty())
            return;
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
            {"fontFamily", QFontInfo(QGuiApplication::font()).family()}, {"bubbles", bubbles}}).toJson());
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
            captureAggregateScene(window, scenario);
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
            captureAggregateScene(window, scenario);
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
            captureAggregateScene(window, scenario, metrics);
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
            if (path.endsWith("/server_settings"))
                return Response{QJsonObject{{"result", "success"}, {"msg", ""},
                    {"realm_name", "Acme Realm"}, {"registration_url", "https://reg.example/signup"}}};
            auto response = sessionResponse(request);
            if (path.endsWith("/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", verifiedProfile}};
            return response;
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.setPreferences(&preferences);
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
        // The organization step gates the credential fields behind discovery (issue #58).
        QVERIFY(!item(content, "userInput"));
        QVERIFY(!item(content, "passwordInput"));
        click(window, item(content, "organizationNextButton"));
        QTRY_VERIFY(item(content, "realmPreviewCard"));
        QCOMPARE(item(content, "realmPreviewName")->property("text").toString(), QString("Acme Realm"));
        QTRY_VERIFY(item(content, "userInput"));
        item(content, "userInput")->setProperty("text", "synthetic-user");
        item(content, "passwordInput")->setProperty("text", "synthetic-password");
        click(window, item(content, "signInButton"));
        QTRY_COMPARE(controller.projects().size(), 1);
        QTRY_VERIFY(item(content, "project-" + projectId));
        click(window, item(content, "project-" + projectId));
        QTRY_COMPARE(controller.streams().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(item(content, "profileDisplayName"), 1000);
        // A committed sign-in persists the organization origin for future suggestions.
        QCOMPARE(preferences.organizationHistory(), QStringList{server.origin().toString()});
        QCOMPARE(item(content, "profileDisplayName")->property("text").toString(), QString("Cassi"));
        QCOMPARE(item(content, "profileEmail")->property("text").toString(), QString("cassi@example.invalid"));
        QCOMPARE(item(content, "profileInitial")->property("text").toString(), QString("C"));
        QCOMPARE(item(content, "profileAvatar")->width(), 36);
        QCOMPARE(item(content, "profileDisplayName")->property("textFormat").toInt(), 0);
        QCOMPARE(item(content, "profileEmail")->property("textFormat").toInt(), 0);
        // The switcher renders the live session first (issue #144), so the
        // quick-button logo replaces the former static organizationLogo.
        QTRY_VERIFY(!controller.sessionSwitcherSessions().isEmpty());
        const auto switcherLiveId = controller.sessionSwitcherSessions().first()
            .toMap().value("id").toString();
        QTRY_VERIFY(item(content, "sessionLogo-" + switcherLiveId));
        QTRY_COMPARE(item(content, "sessionLogo-" + switcherLiveId)->property("status").toInt(), 1);
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
        // #16: content-sized bubble widths scale with the resolved font's advances
        // (up to 5 percent versus the reference measurements); heights stay exact.
        const auto advanceBand = [](qreal expected) { return qMax(qreal(3.0), expected * 0.08); };
        QVERIFY(qAbs(codeBubble->width() - 479.078125) <= advanceBand(479.078125));
        QVERIFY(qAbs(plainBubble->width() - 382.703125) <= advanceBand(382.703125));
        QVERIFY(qAbs(peerBubble->width() - 364.28125) <= advanceBand(364.28125));
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
        auto *wantedTopic = item(window->contentItem(), "topic-" + topicId);
        auto *otherTopic = item(window->contentItem(), "topic-" + secondTopicId);
        auto *streamRow = item(window->contentItem(), "stream-" + streamId);
        QVERIFY(wantedTopic && otherTopic && streamRow);
        const auto reactionTopicReady = [&] {
            auto *column = wantedTopic->parentItem();
            if (!column || otherTopic->parentItem() != column || wantedTopic->window() != window
                || otherTopic->window() != window || !wantedTopic->isVisible() || !otherTopic->isVisible()
                || !wantedTopic->isEnabled() || !otherTopic->isEnabled()) return false;
            const QRectF wanted(wantedTopic->mapToScene(QPointF{}), wantedTopic->size());
            const QRectF other(otherTopic->mapToScene(QPointF{}), otherTopic->size());
            const QRectF parent(column->mapToScene(QPointF{}), column->size());
            const auto streamBottom = streamRow->mapToScene(QPointF(0, streamRow->height())).y();
            return wanted.width() > 0 && wanted.height() > 0 && other.width() > 0 && other.height() > 0
                && wanted.top() >= streamBottom && other.top() >= streamBottom
                && parent.contains(wanted) && parent.contains(other) && !wanted.intersects(other);
        };
        QTRY_VERIFY2(reactionTopicReady(), "Topic rows must occupy separate placed cells before mouse input");
        QSignalSpy reactionInputFrame(window, &QQuickWindow::frameSwapped);
        window->update(); QTRY_VERIFY(!reactionInputFrame.isEmpty()); QVERIFY(reactionTopicReady());
        QSignalSpy wantedClicked(wantedTopic, SIGNAL(clicked())), otherClicked(otherTopic, SIGNAL(clicked()));
        QVERIFY(wantedClicked.isValid() && otherClicked.isValid());
        click(window, wantedTopic);
        QCOMPARE(wantedClicked.size(), 1); QCOMPARE(otherClicked.size(), 0);
        QTRY_COMPARE(controller.selectedTopic(), topicId);
        QTRY_COMPARE(controller.messages().size(), 1);
        auto *content = window->contentItem();
        QTRY_VERIFY(item(content, "message-" + messageId));
        auto *bubble = item(content, "message-" + messageId);
        if (scenario == "menu-peer-actions") {
            // Deep link to a visible message anchors it, highlights the row
            // and releases the anchor again without switching topics.
            controller.openMessageLink("ew://open/org/127.0.0.1-"
                + QString::number(server.origin().port())
                + "/project/" + projectId + "/message/" + messageId);
            QTRY_COMPARE_WITH_TIMEOUT(controller.anchorMessageId(), messageId, 3000);
            auto *highlight = item(content, "messageHighlight-" + messageId);
            QTRY_VERIFY(highlight && highlight->isVisible());
        }
        if (scenario.startsWith("menu-")) {
            preferences.setMode(scenario == "menu-source-dark" ? "dark" : "light");
            QTest::mouseClick(window, Qt::RightButton, {}, bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
            auto *menu = window->findChild<QObject *>("messageMenu");
            QTRY_VERIFY(menu && menu->property("visible").toBool());
            auto *copy = item(content, "copyMessageAction");
            auto *reply = item(content, "replyMessageAction");
            auto *edit = item(content, "editMessageAction");
            auto *remove = item(content, "deleteMessageAction");
            auto *copyLink = item(content, "copyMessageLinkAction");
            QVERIFY(copy && reply && edit && remove && copyLink);
            if (scenario == "menu-no-extra-star") {
                QVERIFY2(!window->findChild<QObject *>("starMessageAction"),
                    "Pinned source full caller has no Star action in the message context menu");
            } else if (scenario == "menu-keyboard-reply") {
                // End lands on Copy text; Copy link sits between Reply and
                // Copy text, and arrows skip separators and the peer's hidden
                // Edit/Delete rows. Source has no Star row.
                QTest::keyClick(window, Qt::Key_End);
                QTRY_VERIFY(copy->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Up);
                QTRY_VERIFY(copyLink->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Up);
                QTRY_VERIFY(reply->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_End);
                QTest::keyClick(window, Qt::Key_Home);
                QTRY_VERIFY(reply->hasActiveFocus());
                QTest::keyClick(window, Qt::Key_Return);
                // Reply opens the composer reply session (docs/NATIVE_DESKTOP_REPLIES.md),
                // it no longer inserts a markdown blockquote into the draft.
                QTRY_COMPARE(controller.replyMessageId(), messageId);
                QVERIFY(controller.draft().isEmpty());
                QTRY_VERIFY(!menu->property("visible").toBool());
                QCOMPARE(posts + deletes, 0);
                const bool closed=closeObservedTestWindow(window); closeBeforeEngine.dismiss(); QVERIFY(closed);
                return;
            } else if (scenario == "menu-encrypted-actions") {
                QVERIFY2(!reply->isVisible() && !copy->isVisible() && !edit->isVisible() && !remove->isVisible(),
                    "Locked message bodies must not expose unsupported Reply/Copy/Edit/Delete actions");
                QVERIFY(!copyLink->isVisible());
                QVERIFY(item(content, "moreReactionsButton")->isEnabled());
            } else if (scenario == "menu-peer-actions") {
                QVERIFY(!controller.messages().first().toMap().value("own").toBool());
                QVERIFY2(!edit->isVisible() && !remove->isVisible(), "Source hides Edit/Delete for peer messages");
                QVERIFY(!edit->isEnabled() && !remove->isEnabled());
                QVERIFY(copyLink->isVisible() && copyLink->isEnabled());
            } else if (scenario == "menu-own-actions") {
                QVERIFY(edit->isVisible() && edit->isEnabled());
                QVERIFY(remove->isVisible() && remove->isEnabled());
                // Opening makes Menu visible before its scheduled row polish.
                // Assert the same actual source order after layout, not a timer.
                QTRY_VERIFY2(reply->mapToScene(QPointF()).y() < copy->mapToScene(QPointF()).y(), "Source Reply precedes Copy text");
                QTRY_VERIFY(copyLink->mapToScene(QPointF()).y() < copy->mapToScene(QPointF()).y());
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

    void longCodeTraceAndUnbrokenTokenStayInsideTheBubble()
    {
        // #131 regression: a long monospace exception trace and a long
        // unbroken token painted past the bubble's right edge. In the real
        // window each message body must lay out every line within its own
        // width, keep the bubble inside the chat area, and stay correct after
        // resizing the window and reopening the conversation.
        WindowChannelFixture fixture;
        fixture.sourceScene = true;
        const QString trace = QStringLiteral(
            "Unknown storage exception: LockNotAvailable('canceling statement due to lock timeout') "
            "CONTEXT: process_00_lockwait AAAAAAAAAAAAAAAAA______________________________________LONGTOKEN______________________________________END");
        auto peerRow = message(QStringLiteral("```text\n") + trace + QStringLiteral("\nCONTEXT: retry deadline exceeded\n```"));
        peerRow["user_uuid"] = channelPeer; peerRow["author_uuid"] = channelPeer; peerRow["is_own"] = false;
        peerRow["pinned"] = false; peerRow["reactions"] = QJsonObject{};
        peerRow["reaction_users"] = QJsonObject{}; peerRow["updated_at"] = peerRow.value("created_at");
        auto ownRow = message(QStringLiteral("Startup failed: LockNotAvailable_") + trace + QStringLiteral("_see_above"), returnedId);
        ownRow["user_uuid"] = userId; ownRow["updated_at"] = ownRow.value("created_at");
        ApiClient api(nullptr, true); Session session(&api);
        NativeController controller(&api, &session);
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (request.method == "GET" && request.target.path().endsWith("/messages/")) {
                ++fixture.historyGets;
                const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
                return {topic.isEmpty() || topic == topicId ? QJsonArray{peerRow, ownRow} : QJsonArray{}};
            }
            return fallback(request);
        };
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Bubble overflow window did not settle before engine teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900); QVERIFY(QTest::qWaitForWindowExposed(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); QTRY_VERIFY(controller.channelMembersVerified());
        click(window, visibleItem(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 3);
        click(window, visibleItem(window->contentItem(), "topic-" + topicId));
        QTRY_VERIFY(controller.hasConversation());
        QTRY_VERIFY(!controller.busy());
        const auto waitBody = [&](const QString &id) {
            QTRY_VERIFY2_WITH_TIMEOUT(visibleItem(window->contentItem(), "message-content-" + id) != nullptr,
                QStringLiteral("id=%1 messages=%2 error=%3 historyGets=%4")
                    .arg(id).arg(controller.messages().size()).arg(controller.error())
                    .arg(fixture.historyGets).toUtf8().constData(), 6000);
        };
        waitBody(messageId);
        waitBody(returnedId);
        // The same layout contract the production view implements: the actual
        // item text laid out with the actual item font, wrap mode and width.
        const auto textStaysInside = [&](const QString &id) {
            auto *body = visibleItem(window->contentItem(), "message-content-" + id); QVERIFY(body);
            auto *bubble = visibleItem(window->contentItem(), "message-" + id); QVERIFY(bubble);
            QTextDocument document;
            document.setDocumentMargin(0);
            document.setDefaultFont(body->property("font").value<QFont>());
            document.setHtml(body->property("text").toString());
            QTextOption option;
            // QQuickText::WrapMode aliases the QTextOption::WrapMode values:
            // NoWrap 0, WordWrap 1, WrapAnywhere 3, and Text.Wrap equals
            // WrapAtWordBoundaryOrAnywhere 4 (Qt 6 compatibility mapping).
            switch (body->property("wrapMode").toInt()) {
            case 0: option.setWrapMode(QTextOption::NoWrap); break;
            case 1: option.setWrapMode(QTextOption::WordWrap); break;
            case 3: option.setWrapMode(QTextOption::WrapAnywhere); break;
            default: option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); break;
            }
            document.setDefaultTextOption(option);
            document.setTextWidth(body->width());
            const auto message = QStringLiteral("%1 phase %2: layout %3 over item %4, html %5")
                .arg(id).arg(window->width()).arg(document.size().width()).arg(body->width())
                .arg(body->property("text").toString().left(300));
            QVERIFY2(body->width() > 0 && document.size().width() <= body->width() + 2.0,
                message.toUtf8().constData());
            const QRectF bubbleRect(bubble->mapToScene(QPointF{}), bubble->size());
            const QRectF bodyRect(body->mapToScene(QPointF{}), body->size());
            QVERIFY2(bubbleRect.contains(bodyRect.adjusted(-1, -1, 1, 1)), message.toUtf8().constData());
            QVERIFY2(bubbleRect.right() <= window->width() + 1.0, message.toUtf8().constData());
        };
        textStaysInside(messageId);
        textStaysInside(returnedId);
        // The trace must genuinely wrap to multiple lines, not overflow on one.
        QVERIFY(visibleItem(window->contentItem(), "message-content-" + messageId)
            ->property("contentHeight").toReal() > 40);
        window->resize(900, 700); QTRY_COMPARE(window->width(), 900);
        QTest::qWait(250);
        textStaysInside(messageId);
        textStaysInside(returnedId);
        window->resize(1440, 900); QTRY_COMPARE(window->width(), 1440);
        QTest::qWait(250);
        textStaysInside(messageId);
        textStaysInside(returnedId);
        // Reopen the same conversation: leave it for another topic and return.
        click(window, visibleItem(window->contentItem(), "topic-" + secondTopicId));
        QTRY_VERIFY(controller.selectedTopic() == secondTopicId);
        click(window, visibleItem(window->contentItem(), "topic-" + topicId));
        QTRY_VERIFY(controller.hasConversation());
        QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(visibleItem(window->contentItem(), "message-content-" + messageId));
        QTRY_VERIFY(visibleItem(window->contentItem(), "message-content-" + returnedId));
        textStaysInside(messageId);
        textStaysInside(returnedId);
        window->close();
        closeBeforeEngine.dismiss();
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
        for (int i = 0; i < 130; ++i) history.insert(i, row(i));
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
                sent = row(131, true);
                sent["read"] = false;
                sent["payload"] = request.body.value("payload");
                history.insert(131, sent);
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
                // The climb crossed the 100 px auto-page zone
                // (docs/NATIVE_DESKTOP_AUTO_PAGING.md): the older page arrives
                // on its own, without the button click.
                QTRY_COMPARE(controller.messages().size(), 100);
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
            QVERIFY(controller.canLoadOlder()); // a page still remains after the auto page
            click(window, dialogButton(window->contentItem(), "Load older messages"));
            QTRY_COMPARE(controller.messages().size(), 130);
        } else if (behavior == "own-send-away") {
            item(window->contentItem(), "messageComposer")->setProperty("text", "A locally sent row pins to the tail.");
            QTRY_VERIFY(controller.canSend());
            click(window, item(window->contentItem(), "sendButton"));
            QTRY_COMPARE(controller.messages().size(), 51);
            QTRY_VERIFY(controller.draft().isEmpty());
        } else {
            auto incoming = row(130, behavior == "own-incoming-away");
            incoming["read"] = false;
            history.insert(130, incoming);
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

    // docs/NATIVE_DESKTOP_AUTO_PAGING.md: a wheel that brings the viewport within
    // LOAD_MORE_THRESHOLD = 100 px of the loaded top (workspace-message-list-scroll
    // .hook.ts:22,1185-1197 at 61dacef78dab2f5d0be76f283bebf49fe29b84af) issues the
    // older-page request with no button click; it never fires before the user
    // scrolls, never re-fires while the viewport stays inside the zone, and re-arms
    // only after the viewport returns past 100 px (:1172-1174).
    void scrollNearTopAutoLoadsOlderPages()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        QMap<int, QJsonObject> history;
        const auto uuidOf = [](int number) {
            return QString("81000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'));
        };
        const auto row = [&](int number) {
            auto entry = message(QString("Paging fixture %1").arg(number), uuidOf(number));
            entry["created_at"] = QDateTime::fromString("2026-01-02T10:00:00.000Z", Qt::ISODateWithMs)
                .addSecs(number).toString(Qt::ISODateWithMs);
            return entry;
        };
        for (int i = 0; i < 120; ++i) history.insert(i, row(i));
        bool holdSecondPage = true;
        QVector<std::function<void()>> heldSecondPage;
        QVector<QPair<QString, QString>> pageGets; // (page_marker, sort_dir) per history page GET
        const auto streamDto = [&] {
            return QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Paging fixture"},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", uuidOf(119)}};
        };
        const auto topicDto = [&] {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Auto paging"}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", uuidOf(119)}};
        };
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return Response{QJsonArray{streamDto()}};
            if (path.endsWith("/streams/" + streamId)) return Response{streamDto()};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) return Response{QJsonArray{topicDto()}};
            if (path.endsWith("/stream_topics/" + topicId)) return Response{topicDto()};
            if (path.endsWith("/messages/") && request.method == "GET") {
                const auto query = QUrlQuery(request.target);
                const auto marker = query.queryItemValue("page_marker");
                if (query.queryItemValue("topic_uuid") == topicId)
                    pageGets.append({marker, query.queryItemValue("sort_dir")});
                // Newest-first pages over 120 rows: tail 70..119, then 20..69, then 0..19.
                const int top = marker.isEmpty() ? 120 : marker == uuidOf(70) ? 70 : marker == uuidOf(20) ? 20 : -1;
                if (top < 0) return Response{QJsonObject{}, 404};
                QJsonArray page;
                for (int number = top - 1; number >= top - 50 && number >= 0; --number) page.append(history.value(number));
                Response response{page};
                if (top == 120) response.headers.insert("X-Pagination-Marker", uuidOf(70).toUtf8());
                else if (top == 70) {
                    response.headers.insert("X-Pagination-Marker", uuidOf(20).toUtf8());
                    if (holdSecondPage)
                        response.defer = [&heldSecondPage](std::function<void()> send) { heldSecondPage.append(send); };
                }
                return response;
            }
            if (path.endsWith("/actions/read_up_to/invoke")) {
                const auto id = path.section('/', -4, -4);
                auto boundary = std::find_if(history.begin(), history.end(), [&id](const auto &entry) {
                    return entry.value("uuid") == id;
                });
                if (boundary == history.end()) return Response{QJsonObject{}, 404};
                for (auto it = history.begin(); it.key() <= boundary.key(); ++it)
                    it.value()["read"] = true;
                return Response{boundary.value()};
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
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Auto-paging window did not settle creation before teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        click(window, item(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, item(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 50);
        auto *list = item(window->contentItem(), "messageList");
        QVERIFY(list);
        QTRY_VERIFY(!list->property("positioning").toBool());
        QTRY_VERIFY(list->property("atYEnd").toBool());
        QCOMPARE(pageGets.size(), 1);
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
                return !list->property("moving").toBool() && stable.elapsed() >= 50;
            }, 1500);
        };
        const auto distanceFromTop = [&] {
            return list->property("contentY").toReal() - list->property("originY").toReal()
                + list->property("topMargin").toReal();
        };
        // Opening a mid-conversation window is programmatic motion only: the
        // reference userScrollSeen gate (handleScroll :1145-1149) forbids any
        // older-page request before a real (non-positioning) scroll movement.
        QTest::qWait(300);
        QCOMPARE(pageGets.size(), 1);
        QVERIFY(controller.canLoadOlder());
        const auto center = list->mapToScene(QPointF(list->width() / 2, list->height() / 2));
        QTest::mouseMove(window, center.toPoint());
        int steps = 0;
        while (pageGets.size() < 2 && steps < 20) {
            QTest::wheelEvent(window, center, QPoint(0, 1200));
            QVERIFY(settleWheel());
            ++steps;
        }
        QCOMPARE(pageGets.size(), 2); // scrolling alone requested the older page, no button click
        QCOMPARE(pageGets.at(1).first, uuidOf(70));
        QCOMPARE(pageGets.at(1).second, QString("desc"));
        QVERIFY(distanceFromTop() <= 100);
        // The held page is still in flight while the viewport stays inside the
        // trigger zone: extra wheel input must not fire a second request.
        for (int extra = 0; extra < 3; ++extra) {
            QTest::wheelEvent(window, center, QPoint(0, 1200));
            QVERIFY(settleWheel());
        }
        QTest::qWait(300);
        QCOMPARE(pageGets.size(), 2);
        QCOMPARE(controller.messages().size(), 50);
        QCOMPARE(heldSecondPage.size(), 1);
        heldSecondPage.takeFirst()();
        QTRY_COMPARE(controller.messages().size(), 100);
        // The preserved viewport sits past 100 px again, so continuing to wheel to
        // the new top re-arms the trigger and fetches the final page.
        steps = 0;
        while (pageGets.size() < 3 && steps < 40) {
            QTest::wheelEvent(window, center, QPoint(0, 1200));
            QVERIFY(settleWheel());
            ++steps;
        }
        QCOMPARE(pageGets.size(), 3);
        QTRY_COMPARE(controller.messages().size(), 120);
        QVERIFY(!controller.canLoadOlder());
        for (int extra = 0; extra < 3; ++extra) {
            QTest::wheelEvent(window, center, QPoint(0, 1200));
            QVERIFY(settleWheel());
        }
        QCOMPARE(pageGets.size(), 3); // exhausted history stops the walk cleanly
        const bool closed = closeIncubatedTestWindow(engine, window);
        if (closed) closeBeforeEngine.dismiss();
        QVERIFY(closed);
    }

    void programmaticAnchorJumpDoesNotAutoPage()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        QMap<int, QJsonObject> history;
        const auto uuidOf = [](int number) {
            return QString("81000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'));
        };
        const auto row = [&](int number) {
            auto entry = message(QString("Anchor jump fixture %1").arg(number), uuidOf(number));
            entry["created_at"] = QDateTime::fromString("2026-01-02T10:00:00.000Z", Qt::ISODateWithMs)
                .addSecs(number).toString(Qt::ISODateWithMs);
            return entry;
        };
        for (int i = 0; i < 120; ++i) history.insert(i, row(i));
        QVector<QPair<QString, QString>> pageGets; // (page_marker, sort_dir) per history page GET
        const auto streamDto = [&] {
            return QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Anchor jump"},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", uuidOf(119)}};
        };
        const auto topicDto = [&] {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Anchor jump"}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", uuidOf(119)}};
        };
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return Response{QJsonArray{streamDto()}};
            if (path.endsWith("/streams/" + streamId)) return Response{streamDto()};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) return Response{QJsonArray{topicDto()}};
            if (path.endsWith("/stream_topics/" + topicId)) return Response{topicDto()};
            if (path.endsWith("/messages/") && request.method == "GET") {
                const auto query = QUrlQuery(request.target);
                const auto marker = query.queryItemValue("page_marker");
                if (query.queryItemValue("topic_uuid") == topicId)
                    pageGets.append({marker, query.queryItemValue("sort_dir")});
                // Newest-first pages over 120 rows: tail 70..119, then 20..69.
                const int top = marker.isEmpty() ? 120 : marker == uuidOf(70) ? 70 : -1;
                if (top < 0) return Response{QJsonObject{}, 404};
                QJsonArray page;
                for (int number = top - 1; number >= top - 50 && number >= 0; --number)
                    page.append(history.value(number));
                Response response{page};
                if (top == 120) response.headers.insert("X-Pagination-Marker", uuidOf(70).toUtf8());
                return response;
            }
            if (path.endsWith("/actions/read_up_to/invoke")) {
                const auto id = path.section('/', -4, -4);
                auto boundary = std::find_if(history.begin(), history.end(), [&id](const auto &entry) {
                    return entry.value("uuid") == id;
                });
                if (boundary == history.end()) return Response{QJsonObject{}, 404};
                for (auto it = history.begin(); it.key() <= boundary.key(); ++it)
                    it.value()["read"] = true;
                return Response{boundary.value()};
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
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Anchor-jump window did not settle creation before teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        click(window, item(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, item(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.messages().size(), 50);
        auto *list = item(window->contentItem(), "messageList");
        QVERIFY(list);
        QTRY_VERIFY(!list->property("positioning").toBool());
        QTRY_VERIFY(list->property("atYEnd").toBool());
        QCOMPARE(pageGets.size(), 1);
        QVERIFY(controller.canLoadOlder());
        // The anchor highlight jump (the code path behind controller.anchorMessageId)
        // moves contentY onto the oldest loaded row, i.e. inside the 100 px trigger
        // zone, without any user scrolling: the reference suppresses handleScroll
        // while anchorHandoffPending, so this must neither mark the seen flag nor
        // request a page.
        QVERIFY(QMetaObject::invokeMethod(list, "anchorToMessage", Q_ARG(QVariant, uuidOf(70))));
        QTRY_VERIFY(list->property("contentY").toReal() - list->property("originY").toReal()
                    + list->property("topMargin").toReal() <= 100.0);
        QTest::qWait(300);
        QCOMPARE(pageGets.size(), 1);
        QVERIFY(!list->property("userScrollSeen").toBool());
    }

    // docs/NATIVE_DESKTOP_AUTO_PAGING.md: when every row of the loaded window is
    // filtered out by account relevance (the reference zero-visible-window effect,
    // workspace-message-list.ui.tsx:998-1022 at 61dacef78dab2f5d0be76f283bebf49
    // fe29b84af: "an invisible page has no scroll surface from which to request
    // the next one"), the list advances pages on its own, older direction first.
    void zeroVisibleWindowAutoAdvances_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("tail") << QString("tail");
        QTest::newRow("anchor") << QString("anchor");
    }

    void zeroVisibleWindowAutoAdvances()
    {
        QFETCH(QString, scenario);
        const bool anchor = scenario == "anchor";
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        MockServer server;
        const auto uuidOf = [](int number) {
            return QString("81000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'));
        };
        const auto ordinaryRow = [&](int number) {
            auto entry = message(QString("Ordinary fixture %1").arg(number), uuidOf(number));
            entry["author_uuid"] = QString("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
            entry["is_own"] = false;
            entry["created_at"] = QDateTime::fromString("2026-01-02T10:00:00.000Z", Qt::ISODateWithMs)
                .addSecs(number).toString(Qt::ISODateWithMs);
            return entry;
        };
        const auto announcedRow = [&](int number) {
            auto entry = ordinaryRow(number);
            entry["payload"] = QJsonObject{{"kind", "key_announced"},
                {"key_uuid", QString("77000000-0000-4000-8000-%1").arg(number, 12, 10, QLatin1Char('0'))}};
            return entry;
        };
        QVector<QPair<QString, QString>> pageGets; // (page_marker, sort_dir) per history page GET
        const auto streamDto = [&] {
            return QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Service fixture"},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", anchor ? uuidOf(60) : uuidOf(119)}};
        };
        const auto topicDto = [&] {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Announcement run"}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", anchor ? uuidOf(60) : uuidOf(119)}};
        };
        const auto withMarker = [](Response response, const QByteArray &marker) {
            response.headers.insert("X-Pagination-Marker", marker);
            return response;
        };
        server.handler = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/")) return Response{QJsonArray{streamDto()}};
            if (path.endsWith("/streams/" + streamId)) return Response{streamDto()};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.endsWith("/stream_topics/")) return Response{QJsonArray{topicDto()}};
            if (path.endsWith("/stream_topics/" + topicId)) return Response{topicDto()};
            if (path.endsWith("/messages/") && request.method == "GET") {
                const auto query = QUrlQuery(request.target);
                const auto wanted = query.queryItemValue("uuid");
                if (!wanted.isEmpty()) {
                    if (anchor && wanted == uuidOf(60)) return Response{QJsonArray{announcedRow(60)}};
                    return Response{QJsonArray{}};
                }
                const auto marker = query.queryItemValue("page_marker");
                const auto dir = query.queryItemValue("sort_dir");
                if (query.queryItemValue("topic_uuid") == topicId)
                    pageGets.append({marker, dir});
                if (!anchor) {
                    // Tail window is all announcements; two older pages are
                    // announcements too, the third brings ordinary rows.
                    if (marker.isEmpty())
                        return withMarker(Response{QJsonArray{announcedRow(119), announcedRow(118),
                            announcedRow(117), announcedRow(116)}}, "svc-older-1");
                    if (marker == "svc-older-1" && dir == "desc")
                        return withMarker(Response{QJsonArray{announcedRow(115), announcedRow(114),
                            announcedRow(113), announcedRow(112)}}, "svc-older-2");
                    if (marker == "svc-older-2" && dir == "desc")
                        return Response{QJsonArray{ordinaryRow(100), ordinaryRow(101)}};
                    return Response{QJsonArray{}, 404};
                }
                // Anchor around the announcement row 60: before = announcements
                // (older end), first newer page = announcements with a marker,
                // second newer page = ordinary rows.
                if (dir == "desc" && marker == uuidOf(60))
                    return Response{QJsonArray{announcedRow(58), announcedRow(59)}};
                if (dir == "asc" && marker == uuidOf(60))
                    return withMarker(Response{QJsonArray{announcedRow(61), announcedRow(62)}}, "newer-2");
                if (dir == "asc" && marker == "newer-2")
                    return Response{QJsonArray{ordinaryRow(63), ordinaryRow(64)}};
                return Response{QJsonArray{}}; // a bounded walk fallback: stays invisible, test fails
            }
            if (path.endsWith("/actions/read_up_to/invoke")) return Response{QJsonObject{}};
            return sessionResponse(request);
        };
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Zero-visible window did not settle creation before teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(server.origin(), "synthetic-user", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1);
        session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        if (!anchor) {
            click(window, item(window->contentItem(), "stream-" + streamId));
            QTRY_COMPARE(controller.topics().size(), 1);
            click(window, item(window->contentItem(), "topic-" + topicId));
            QTRY_VERIFY(controller.messages().size() >= 4);
        } else {
            click(window, item(window->contentItem(), "stream-" + streamId));
            QTRY_COMPARE(controller.topics().size(), 1);
            controller.openMessageInChat(uuidOf(60));
            QTRY_COMPARE(controller.anchorMessageId(), uuidOf(60));
            QTRY_VERIFY(!controller.messages().isEmpty());
        }
        QTRY_VERIFY(!controller.busy());
        // Every row on the loaded window is an account-irrelevant announcement
        // (accountRelevant returns false for key_announced), so no bubble renders.
        QTest::qWait(300);
        QVERIFY(!item(window->contentItem(), "message-" + uuidOf(anchor ? 60 : 119)));
        // No scroll input happened: the first additional page must arrive on its
        // own. The tail window has no newer boundary so it advances older; the
        // anchor window has one and must prefer it (reference direction order).
        if (!anchor) {
            QTRY_VERIFY(pageGets.size() >= 2);
            QCOMPARE(pageGets.at(1).first, QString("svc-older-1"));
            QCOMPARE(pageGets.at(1).second, QString("desc"));
            QTRY_COMPARE(pageGets.size(), 3);
            QTRY_COMPARE(controller.messages().size(), 10);
        } else {
            QTRY_VERIFY(std::any_of(pageGets.cbegin(), pageGets.cend(), [](const auto &entry) {
                return entry.first == "newer-2" && entry.second == "asc";
            }));
        }
        // An ordinary row ends the auto-advance: it renders and nothing further loads.
        QTRY_VERIFY(item(window->contentItem(), "message-" + uuidOf(anchor ? 63 : 100)));
        const auto settledGets = pageGets.size();
        QTest::qWait(400);
        QCOMPARE(pageGets.size(), settledGets);
        const bool closed = closeIncubatedTestWindow(engine, window);
        if (closed) closeBeforeEngine.dismiss();
        QVERIFY(closed);
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

    void lastConversationRestoresInANewActualMainAfterSettingsReopen()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        WindowChannelFixture fixture;
        fixture.sourceScene = true;
        const auto fallback = fixture.server.handler;
        int canonicalTopicLookups = 0;
        QVector<QUrl> planningHistory;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto route = request.target.path();
            if (route == "/api/workspace/v1/messenger/stream_topics/" + secondTopicId) {
                ++canonicalTopicLookups;
                auto topic = windowChannelTopic(secondTopicId, streamId, "Planning");
                topic["is_default"] = false;
                return {topic};
            }
            if (route.endsWith("/messages/") && request.method == "GET") {
                const QUrlQuery query(request.target);
                if (query.queryItemValue("topic_uuid") != secondTopicId) return {QJsonArray{}};
                planningHistory.append(request.target);
                auto row = message("Planning restored through the real message view.", messageId, secondTopicId);
                row["author_uuid"] = channelPeer; row["is_own"] = false;
                return {QJsonArray{row}};
            }
            if (route.endsWith("/streams/") && request.method == "GET") {
                auto stream = windowChannel(streamId, "Product team");
                stream["default_topic_uuid"] = topicId;
                return {QJsonArray{stream}};
            }
            return fallback(request);
        };
        const auto owner = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{
            fixture.server.origin().toString(QUrl::FullyEncoded), projectId, userId}).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256).toHex());
        for (int visit = 0; visit != 2; ++visit) {
            NativePreferences preferences(path);
            ApiClient api(nullptr, true);
            Session session(&api);
            NativeController controller(&api, &session);
            QQmlApplicationEngine engine;
            loadNativeUi(engine, &controller, &preferences);
            QVERIFY(!engine.rootObjects().isEmpty());
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            QVERIFY(window);
            auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
                if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                    QTest::qFail("Saved conversation Main teardown did not settle", __FILE__, __LINE__);
            });
            QVERIFY(QTest::qWaitForWindowExposed(window));
            auto *content = window->contentItem();
            QVERIFY(controller.selectedStream().isEmpty());
            QVERIFY(controller.selectedTopic().isEmpty());
            controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            QTRY_COMPARE(controller.projects().size(), 1);
            controller.openProject(projectId);
            if (visit == 0) {
                QTRY_VERIFY(item(content, "stream-" + streamId));
                QTRY_VERIFY(!controller.busy());
                QVERIFY(controller.selectedStream().isEmpty());
                QSignalSpy frame(window, &QQuickWindow::frameSwapped);
                QVERIFY(frame.isValid());
                window->update(); QTRY_VERIFY(!frame.isEmpty());
                click(window, item(content, "stream-" + streamId));
                QTRY_COMPARE(controller.selectedStream(), streamId);
                QTRY_VERIFY(item(content, "topic-" + secondTopicId));
                frame.clear(); window->update(); QTRY_VERIFY(!frame.isEmpty());
                click(window, item(content, "topic-" + secondTopicId));
            }
            QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), secondTopicId, 2000);
            QCOMPARE(controller.selectedStream(), streamId);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
            QTRY_VERIFY_WITH_TIMEOUT(controller.topicsComplete(), 2000);
            QTRY_VERIFY(visibleItem(content, "message-content-" + messageId));
            QVERIFY(visibleItem(content, "message-content-" + messageId)->property("text").toString()
                .contains("Planning restored through the real message view."));
            QTRY_VERIFY(item(content, "messageComposer")->isEnabled());
            QCOMPARE(preferences.loadLastConversation(owner), (QStringList{streamId, secondTopicId}));
            const bool closed = closeIncubatedTestWindow(engine, window);
            if (closed) close.dismiss();
            QVERIFY(closed);
        }
        QCOMPARE(canonicalTopicLookups, 1); // Only the fresh Main needs canonical restoration.
        QCOMPARE(planningHistory.size(), 2);
        for (const auto &target : planningHistory) {
            const QUrlQuery query(target);
            QCOMPARE(query.queryItemValue("stream_uuid"), streamId);
            QCOMPARE(query.queryItemValue("topic_uuid"), secondTopicId);
        }
        QCOMPARE(fixture.posts, 0);
        QCOMPARE(fixture.memberPosts, 0);
    }

    void rememberedAccountResumesAndRemovesThroughVisibleQuickControls_data()
    {
        QTest::addColumn<bool>("startup");
        QTest::newRow("manual-continue") << false;
        QTest::newRow("automatic-startup") << true;
    }

    void rememberedAccountResumesAndRemovesThroughVisibleQuickControls()
    {
        QFETCH(bool, startup);
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
        if (startup) QVERIFY(vault.setCurrentAccountId(saved.id).isEmpty());
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
        if (startup) {
            QTimer::singleShot(0, &session, &Session::resumeCurrentAccount);
            QTRY_VERIFY(controller.busy());
        } else click(window, item(content, "resume-" + saved.id));
        QVERIFY(controller.busy());
        QVERIFY(!item(content, "resume-" + saved.id)->isEnabled());
        QVERIFY(!item(content, "forget-" + saved.id)->isEnabled());
        QVERIFY(!item(content, "serverInput")->isEnabled());
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
        QTRY_COMPARE(vault.currentAccountId(), saved.id);
        QVERIFY(!item(content, "passwordInput")); // credentials are unmounted before the wizard advances
        QVERIFY(!item(content, "messageComposer")->isEnabled());
        click(window, item(content, "accountMenuButton"));
        QTRY_VERIFY(item(content, "signOutButton")->isVisible());
        click(window, item(content, "signOutButton"));
        QTRY_VERIFY(!controller.authenticated());
        QTRY_VERIFY(controller.savedAccounts().isEmpty());
        QTRY_VERIFY(backend.values.isEmpty());
        QVERIFY(vault.currentAccountId().isEmpty());
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
            QVERIFY(send);
            // Padded composer region per the reference at workspace_ui 61dacef:
            // message-composer.ui.tsx:2040 `relative flex min-h-0 flex-1 flex-col p-2`
            // wrapping the :2119-2121 input row `flex min-h-0 flex-1 items-end gap-3`
            // whose input box is `min-h-10` (40px). Border-box height of the region:
            // 1px composer rule + p-2 (8x2) + 40 = 57. The #122 restructure inserted
            // the toolbar row and an inner row wrapper, so the old send grandparent
            // now lands on the 40px input row; anchor to the stable objectNames.
            auto *region = item(content, "messageComposerShell");
            auto *inputColumn = item(content, "composerInputColumn");
            QVERIFY(region && inputColumn);
            QCOMPARE(region->height(), qreal(57));
            QCOMPARE(inputColumn->height(), qreal(40));
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

    void receivedStandaloneImagePaintsBetweenTextAndCaption()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString fileId = "99999999-9999-4999-8999-999999999991";
        const auto png = avatarProbePng(false); // Existing public 96x64 fixture, never passed to Main.
        QCOMPARE(png.size(), qsizetype(230));
        QCOMPARE(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex(),
            QByteArray("9fb39a2b2737f88bdf0b383c4fcea4b0f3636e584b280f10afd18444739ed1f9"));
        const auto expected = QImage::fromData(png, "PNG");
        QVERIFY(!expected.isNull()); QCOMPARE(expected.size(), QSize(96, 64));
        const QString urn = "urn:image:" + fileId
            + "?name=inline.png&content_type=image%2Fpng&w=96&h=64&size=230";
        const QString markdown = "Before image\n\n![inline.png](" + urn + ")\n\nAfter image";
        auto canonical = message(markdown);
        canonical["user_uuid"] = userId; // Source's canonical message validator also requires the sender identity.
        QCOMPARE(canonical.value("user_uuid").toString(), userId);
        canonical["author_uuid"] = channelPeer; canonical["is_own"] = false;
        canonical["pinned"] = false; canonical["reactions"] = QJsonObject{};
        canonical["reaction_users"] = QJsonObject{}; canonical["updated_at"] = canonical.value("created_at");
        auto metadata = avatarProbeMetadata(fileId, png);
        metadata["stream_uuid"] = streamId; metadata["name"] = "inline.png";
        int metadataGets = 0, binaryGets = 0, servedHistory = 0, messagePosts = 0, fileWrites = 0;
        bool fileHeadersOkay = true;
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const QString filePath = "/api/workspace/v1/messenger/files/" + fileId;
            if (path.startsWith("/api/workspace/v1/messenger/files/")) {
                fileHeadersOkay = fileHeadersOkay
                    && request.headers.value("authorization") == QByteArray("Bearer synthetic-project-token")
                    && request.headers.value("cookie").isEmpty();
                if (request.method != "GET") { ++fileWrites; return Response{QJsonObject{}, 405}; }
                if (path == filePath) { ++metadataGets; return Response{metadata}; }
                if (path == filePath + "/actions/download") {
                    ++binaryGets; Response result; result.rawBody = png; result.rawContentType = "image/png";
                    return result;
                }
                return Response{QJsonObject{}, 404};
            }
            if (path.endsWith("/messages/") && request.method == "POST") {
                ++messagePosts; return Response{QJsonObject{}, 405};
            }
            if (path.endsWith("/messages/") && request.method == "GET") {
                ++fixture.historyGets;
                const QUrlQuery query(request.target);
                if (query.queryItemValue("stream_uuid") == streamId
                    && query.queryItemValue("topic_uuid") == topicId) {
                    ++servedHistory; return Response{QJsonArray{canonical}};
                }
                return Response{QJsonArray{}};
            }
            return fallback(request);
        };
        ApiClient api(nullptr, true); Session session(&api); FileClient files(&api);
        MessageImages images(&api, &session, &files);
        NativeController controller(&api, &session); controller.useFiles(&files);
        NativePreferences preferences(directory.filePath("received-image.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences, nullptr, nullptr, &images);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Received image window did not settle before engine teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900); QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QCOMPARE(window->devicePixelRatio(), qreal(1)); QCOMPARE(window->size(), QSize(1440, 900));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); QTRY_VERIFY(controller.channelMembersVerified());
        preferences.setOwner(controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QCOMPARE(preferences.locale(), QString("en")); QCOMPARE(preferences.mode(), QString("light"));
        QTRY_VERIFY(visibleItem(window->contentItem(), "stream-" + streamId));
        click(window, visibleItem(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        QTRY_VERIFY(visibleItem(window->contentItem(), "topic-" + topicId));
        click(window, visibleItem(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.selectedStream(), streamId); QTRY_COMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.conversationKind(), QString("topic")); QVERIFY(controller.hasConversation());
        QTRY_VERIFY(!controller.busy()); QTRY_VERIFY(servedHistory > 0);
        const auto canonicalLoaded = [&] {
            const auto rows = controller.messages();
            return rows.size() == 1 && rows.first().toMap().value("id").toString() == messageId
                && rows.first().toMap().value("kind").toString() == "markdown"
                && rows.first().toMap().value("content").toString() == markdown
                && rows.first().toMap().value("authorId").toString() == channelPeer;
        };
        QTRY_VERIFY(canonicalLoaded());
        NativeMarkdown parser; const auto reference = parser.reference(urn);
        QCOMPARE(reference.value("kind").toString(), QString("image"));
        QCOMPARE(reference.value("id").toString(), fileId);
        QCOMPARE(reference.value("width").toInt(), 96); QCOMPARE(reference.value("height").toInt(), 64);
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        auto *viewport = item(window->contentItem(), "messageList"); QVERIFY(viewport);
        QTRY_VERIFY(!viewport->property("positioning").toBool());
        const auto textVisible = [&] {
            auto *body = visibleItem(window->contentItem(), "message-content-" + messageId);
            if (!body) return false;
            const auto text = body->property("text").toString();
            return text.contains("Before image") && text.contains("After image");
        };
        QTRY_VERIFY(textVisible());
        QSignalSpy frame(window, &QQuickWindow::frameSwapped); QVERIFY(frame.isValid());
        window->update(); QTRY_VERIFY(!frame.isEmpty());
        QRect paintedImage, sampledBubble; int matchingPixels = 0; bool pixelOraclePassed = false;
        const auto closeColor = [](const QColor &a, const QColor &b) {
            return a.isValid() && b.isValid() && qAbs(a.red() - b.red()) <= 3
                && qAbs(a.green() - b.green()) <= 3 && qAbs(a.blue() - b.blue()) <= 3;
        };
        const QVector<QPoint> probes{{8, 8}, {24, 16}, {72, 16}, {24, 48}, {72, 48}, {88, 56}};
        const auto pixelsPainted = [&] {
            paintedImage = {}; sampledBubble = {}; matchingPixels = 0;
            auto *bubble = visibleItem(window->contentItem(), "message-" + messageId);
            if (!bubble || bubble->window() != window) return false;
            const auto pixels = window->grabWindow(); if (pixels.isNull()) return false;
            sampledBubble = QRectF(bubble->mapToScene(QPointF{}), bubble->size()).toAlignedRect();
            if (!pixels.rect().contains(sampledBubble)) return false;
            for (int y = sampledBubble.top(); y <= sampledBubble.bottom(); ++y) {
                for (int x = sampledBubble.left(); x <= sampledBubble.right(); ++x) {
                    const auto color = pixels.pixelColor(x, y);
                    bool known = false;
                    for (const auto &point : probes) if (closeColor(color, expected.pixelColor(point))) { known = true; break; }
                    if (known) { paintedImage |= QRect(x, y, 1, 1); ++matchingPixels; }
                }
            }
            if (qAbs(paintedImage.width() - 96) > 1 || qAbs(paintedImage.height() - 64) > 1
                || matchingPixels < 5500) return false;
            for (const auto &point : probes) {
                const QPoint sample(paintedImage.left() + qFloor((point.x() + .5) * paintedImage.width() / 96),
                    paintedImage.top() + qFloor((point.y() + .5) * paintedImage.height() / 64));
                if (!closeColor(pixels.pixelColor(sample), expected.pixelColor(point))) return false;
            }
            return true;
        };
        const auto rect = [](const QRect &value) -> QJsonObject {
            return {{"x", value.x()}, {"y", value.y()}, {"width", value.width()}, {"height", value.height()}};
        };
        const auto evidence = [&](const char *phase) {
            qInfo().noquote() << "RECEIVED_IMAGE_PIXELS" << QJsonDocument(QJsonObject{
                {"phase", QString::fromLatin1(phase)}, {"canonicalLoaded", canonicalLoaded()}, {"pixelOraclePassed", pixelOraclePassed},
                {"servedHistory", servedHistory}, {"metadataGets", metadataGets}, {"binaryGets", binaryGets},
                {"messagePosts", messagePosts}, {"fileWrites", fileWrites}, {"fileHeadersOkay", fileHeadersOkay},
                {"locale", preferences.locale()}, {"mode", preferences.mode()}, {"dpr", window->devicePixelRatio()},
                {"windowWidth", window->width()}, {"windowHeight", window->height()},
                {"bubble", rect(sampledBubble)}, {"imagePixels", rect(paintedImage)}, {"matchingPixels", matchingPixels}
            }).toJson(QJsonDocument::Compact);
        };
        pixelsPainted(); evidence("before-pixel-oracle");
        const auto failedPixelEvidence = qScopeGuard([&] { if (QTest::currentTestFailed()) evidence("failed-case"); });
        // First intended old-product failure: real PNG pixels, not a future
        // objectName/Ready flag, a missing API, or a failed-transfer assumption.
        QTRY_VERIFY2(pixelsPainted(), "The actual 96x64 received PNG, including both edge guard bands, must paint inside its message bubble");
        pixelOraclePassed = true; evidence("image-painted");
        QCOMPARE(metadataGets, 1); QCOMPARE(binaryGets, 1); QVERIFY(fileHeadersOkay);
        QCOMPARE(messagePosts, 0); QCOMPARE(fileWrites, 0); QCOMPARE(fixture.posts + fixture.memberPosts, 0);
        QTRY_VERIFY(canonicalLoaded()); QVERIFY(textVisible());
        auto *body = visibleItem(window->contentItem(), "message-content-" + messageId); QVERIFY(body);
        const QRectF bodyBounds(body->mapToScene(QPointF{}), body->size());
        QVERIFY(bodyBounds.contains(QRectF(paintedImage)));
        // Actual pinned Source body capture, same complete handler DTO/PNG:
        // 20px paragraph + 14px blank line + 82px image paragraph + 14px
        // blank line + 22px caption including inline time = 152px. Compare
        // relative body geometry, not the synthetic host's absolute position.
        QCOMPARE(qRound(bodyBounds.height()), 152);
        QCOMPARE(qRound(paintedImage.left() - bodyBounds.left()), 1);
        QCOMPARE(qRound(paintedImage.top() - bodyBounds.top()), 43);
        const auto roundedPixels = window->grabWindow();
        QVERIFY(!roundedPixels.isNull());
        // The same raw Source frame has white outer corner pixels from its
        // 4px image radius; keep the rectangular file/provider bytes intact.
        for (const auto &corner : {paintedImage.topLeft(), paintedImage.topRight(),
                                  paintedImage.bottomLeft(), paintedImage.bottomRight()})
            QVERIFY2(closeColor(roundedPixels.pixelColor(corner), QColor("#ffffff")),
                "Source's rounded image corners must reveal this peer bubble's white background");
        // Existing body keeps the actual preceding and trailing text rows;
        // the image is not appended outside the body or floated over the caption.
        QVERIFY(paintedImage.top() - bodyBounds.top() >= 16);
        QVERIFY(bodyBounds.bottom() - (paintedImage.bottom() + 1) >= 16);
        QVERIFY(!images.imageUrl(urn).isEmpty());
        QCOMPARE(images.info(urn), (QVariantMap{{"width", 96}, {"height", 64}}));
        QCOMPARE(images.stats().leases, 1); QCOMPARE(images.stats().previews, 1);
        QCOMPARE(images.stats().previewBytes, qint64(96 * 64 * 4));
        const auto captureDirectory = qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");
        if (!captureDirectory.isEmpty()) {
            QDir capture(captureDirectory); QVERIFY(capture.mkpath("."));
            QVERIFY(window->grabWindow().save(capture.filePath("received-image-en-light.png")));
            QFile dto(capture.filePath("received-image-handler-dto.json")); QVERIFY(dto.open(QIODevice::WriteOnly));
            const auto dtoBytes = QJsonDocument(canonical).toJson(QJsonDocument::Compact);
            QCOMPARE(dto.write(dtoBytes), qint64(dtoBytes.size())); dto.close();

        }
        // A real route change releases this body. Returning reads the retained
        // preview; it must not repeat metadata or binary requests.
        click(window, visibleItem(window->contentItem(), "stream-" + streamId));
        QTRY_VERIFY(!visibleItem(window->contentItem(), "message-content-" + messageId));
        QTRY_COMPARE(images.stats().leases, 0);
        QCOMPARE(images.stats().previews, 1);
        QTRY_VERIFY(visibleItem(window->contentItem(), "topic-" + topicId));
        click(window, visibleItem(window->contentItem(), "topic-" + topicId));
        QTRY_VERIFY(canonicalLoaded()); QTRY_VERIFY(textVisible()); QTRY_VERIFY(pixelsPainted());
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(images.stats().leases, 1); QCOMPARE(metadataGets, 1); QCOMPARE(binaryGets, 1);
        QCOMPARE(messagePosts, 0); QCOMPARE(fileWrites, 0);
        qInfo().noquote() << "RECEIVED_IMAGE_WARM_CACHE" << QJsonDocument(QJsonObject{
            {"metadataGets", metadataGets}, {"binaryGets", binaryGets},
            {"previews", images.stats().previews}, {"previewBytes", double(images.stats().previewBytes)},
            {"pipelineJobs", images.stats().pipelineJobs}, {"leases", images.stats().leases}
        }).toJson(QJsonDocument::Compact);
        const bool closed = closeIncubatedTestWindow(engine, window);
        if (closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
    }
    void receivedBrokenImageKeepsItsAttachmentSaveAction()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString fileId = "99999999-9999-4999-8999-999999999992";
        // Metadata deliberately authenticates these exact bytes. This is a
        // decoder failure, not an HTTP/hash/size failure or a fake Ready state.
        const QByteArray bytes("synthetic invalid PNG preview bytes");
        const QString urn = "urn:image:" + fileId
            + "?name=broken.png&content_type=image%2Fpng&w=96&h=64&size=" + QString::number(bytes.size());
        const QString markdown = "Before broken image\n\n![broken.png](" + urn + ")\n\nAfter broken image";
        auto canonical = message(markdown);
        canonical["user_uuid"] = userId; canonical["author_uuid"] = channelPeer; canonical["is_own"] = false;
        canonical["pinned"] = false; canonical["reactions"] = QJsonObject{};
        canonical["reaction_users"] = QJsonObject{}; canonical["updated_at"] = canonical.value("created_at");
        auto metadata = avatarProbeMetadata(fileId, bytes);
        metadata["stream_uuid"] = streamId; metadata["name"] = "broken.png";
        QCOMPARE(metadata.value("size_bytes").toInt(), int(bytes.size()));
        QCOMPARE(metadata.value("hash").toString(), QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        int metadataGets = 0, binaryGets = 0, servedHistory = 0, messagePosts = 0, fileWrites = 0;
        bool fileHeadersOkay = true;
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const QString filePath = "/api/workspace/v1/messenger/files/" + fileId;
            if (path.startsWith("/api/workspace/v1/messenger/files/")) {
                fileHeadersOkay = fileHeadersOkay
                    && request.headers.value("authorization") == QByteArray("Bearer synthetic-project-token")
                    && request.headers.value("cookie").isEmpty();
                if (request.method != "GET") { ++fileWrites; return Response{QJsonObject{}, 405}; }
                if (path == filePath) { ++metadataGets; return Response{metadata}; }
                if (path == filePath + "/actions/download") {
                    ++binaryGets; Response result; result.rawBody = bytes; result.rawContentType = "image/png";
                    return result;
                }
                return Response{QJsonObject{}, 404};
            }
            if (path.endsWith("/messages/") && request.method == "POST") {
                ++messagePosts; return Response{QJsonObject{}, 405};
            }
            if (path.endsWith("/messages/") && request.method == "GET") {
                ++fixture.historyGets;
                const QUrlQuery query(request.target);
                if (query.queryItemValue("stream_uuid") == streamId
                    && query.queryItemValue("topic_uuid") == topicId) {
                    ++servedHistory; return Response{QJsonArray{canonical}};
                }
                return Response{QJsonArray{}};
            }
            return fallback(request);
        };
        ApiClient api(nullptr, true); Session session(&api); FileClient files(&api);
        MessageImages images(&api, &session, &files);
        NativeController controller(&api, &session); controller.useFiles(&files);
        NativePreferences preferences(directory.filePath("broken-image.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences, nullptr, nullptr, &images);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Broken image window did not settle before engine teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900); QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QCOMPARE(window->devicePixelRatio(), qreal(1)); QCOMPARE(window->size(), QSize(1440, 900));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); QTRY_VERIFY(controller.channelMembersVerified());
        preferences.setOwner(controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QTRY_VERIFY(visibleItem(window->contentItem(), "stream-" + streamId));
        click(window, visibleItem(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        QTRY_VERIFY(visibleItem(window->contentItem(), "topic-" + topicId));
        click(window, visibleItem(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.selectedStream(), streamId); QTRY_COMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.conversationKind(), QString("topic")); QVERIFY(controller.hasConversation());
        QTRY_VERIFY(!controller.busy()); QTRY_VERIFY(servedHistory > 0);
        const auto canonicalLoaded = [&] {
            const auto rows = controller.messages();
            return rows.size() == 1 && rows.first().toMap().value("id").toString() == messageId
                && rows.first().toMap().value("content").toString() == markdown
                && rows.first().toMap().value("authorId").toString() == channelPeer;
        };
        QTRY_VERIFY(canonicalLoaded());
        auto *viewport = item(window->contentItem(), "messageList"); QVERIFY(viewport);
        QTRY_VERIFY(!viewport->property("positioning").toBool());
        QTRY_VERIFY(images.failed(urn));
        QTRY_COMPARE(images.stats().pipelineJobs, 0);
        QCOMPARE(metadataGets, 1); QCOMPARE(binaryGets, 1); QVERIFY(fileHeadersOkay);
        QCOMPARE(images.stats().leases, 1); QCOMPARE(images.stats().previews, 0);
        QVERIFY(images.imageUrl(urn).isEmpty()); QVERIFY(images.info(urn).isEmpty());
        // Reacquire the actual visible component by its existing public
        // reference, never a future objectName or a stale delegate pointer.
        const auto picture = [&]() -> QQuickItem * {
            auto *body = visibleItem(window->contentItem(), "message-content-" + messageId);
            const auto find = [&](auto &&self, QQuickItem *node) -> QQuickItem * {
                if (!node || !node->isVisible()) return nullptr;
                if (node->property("reference").toString() == urn
                    && node->metaObject()->indexOfSignal("opened()") >= 0) return node;
                for (auto *child : node->childItems()) if (auto *found = self(self, child)) return found;
                return nullptr;
            };
            return find(find, body);
        };
        const auto failedPictureVisible = [&] {
            auto *current = picture();
            return current && current->property("failed").toBool();
        };
        QTRY_VERIFY(failedPictureVisible());
        QSignalSpy frame(window, &QQuickWindow::frameSwapped); QVERIFY(frame.isValid());
        window->update(); QTRY_VERIFY(!frame.isEmpty());
        QVERIFY(images.failed(urn)); QVERIFY(images.imageUrl(urn).isEmpty()); QVERIFY(images.info(urn).isEmpty());
        QCOMPARE(metadataGets, 1); QCOMPARE(binaryGets, 1); // Pure reads/render did not retry.
        QPointer<QQuickItem> target = picture(); QVERIFY(target);
        QCOMPARE(target->window(), window); QVERIFY(target->width() > 12 && target->height() > 12);
        auto *retry = dialogButton(target.data(), "Retry"); QVERIFY(retry);
        QVERIFY(retry->isEnabled());
        const QRectF retryBounds(retry->mapToItem(target.data(), QPointF{}), retry->size());
        QPointF local; bool outsideRetry = false;
        for (const auto &point : {QPointF(4, 4), QPointF(target->width() - 4, 4),
                                 QPointF(target->width() - 4, target->height() - 4), QPointF(4, target->height() - 4)}) {
            if (!retryBounds.contains(point)) { local = point; outsideRetry = true; break; }
        }
        QVERIFY(outsideRetry);
        const auto at = target->mapToScene(local).toPoint();
        QVERIFY(QRect(QPoint{}, window->size()).contains(at));
        auto *saveDialog = window->findChild<QObject *>("attachmentSaveDialog"); QVERIFY(saveDialog);
        QVERIFY(!saveDialog->property("visible").toBool());
        QSignalSpy opened(target.data(), SIGNAL(opened())); QVERIFY(opened.isValid());
        QSignalSpy rejected(saveDialog, SIGNAL(rejected())); QVERIFY(rejected.isValid());
        const auto evidence = [&](const char *phase) {
            qInfo().noquote() << "BROKEN_IMAGE_SAVE" << QJsonDocument(QJsonObject{
                {"phase", QString::fromLatin1(phase)}, {"canonicalLoaded", canonicalLoaded()},
                {"failed", images.failed(urn)}, {"urlEmpty", images.imageUrl(urn).isEmpty()},
                {"metadataGets", metadataGets}, {"binaryGets", binaryGets}, {"opened", int(opened.size())},
                {"dialogVisible", saveDialog->property("visible").toBool()}, {"outsideRetry", outsideRetry},
                {"point", QJsonArray{at.x(), at.y()}}, {"fileHeadersOkay", fileHeadersOkay}
            }).toJson(QJsonDocument::Compact);
        };
        auto failureEvidence = qScopeGuard([&] { if (QTest::currentTestFailed()) evidence("failed-case"); });
        evidence("before-one-pointer");
        QTest::mouseClick(window, Qt::LeftButton, {}, at); // One real input, never Retry or a direct callback.
        // First intended current-product failure: a failed preview still names
        // a valid attachment and must retain the existing Save dialog action.
        QTRY_VERIFY2(saveDialog->property("visible").toBool(),
            "A failed received-image preview must keep its attachment Save action outside the Retry button");
        QCOMPARE(opened.size(), 1);
        QCOMPARE(window->property("downloadOwner").toString(), controller.ownerKey());
        QCOMPARE(window->property("downloadId").toString(), fileId);
        QCOMPARE(metadataGets, 1); QCOMPARE(binaryGets, 1); // Opening a chooser downloads nothing.
        QTRY_VERIFY(dialogButton("Cancel"));
        auto *cancel = dialogButton("Cancel"); QVERIFY(cancel && cancel->window());
        click(cancel->window(), cancel); // Existing real Qt fallback dialog input.
        QTRY_COMPARE(rejected.size(), 1); QTRY_VERIFY(!saveDialog->property("visible").toBool());
        QVERIFY(window->property("downloadId").toString().isEmpty());
        QCOMPARE(opened.size(), 1); QCOMPARE(metadataGets, 1); QCOMPARE(binaryGets, 1);
        QVERIFY(images.failed(urn)); QCOMPARE(images.stats().previews, 0);
        QCOMPARE(messagePosts, 0); QCOMPARE(fileWrites, 0); QCOMPARE(fixture.posts + fixture.memberPosts, 0);
        QVERIFY(canonicalLoaded()); evidence("canceled-without-download");
        const bool closed = closeIncubatedTestWindow(engine, window);
        if (closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
    }

    void captionAndPickedImageSendThenPaintOwnInlinePixels()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString fileId = "99999999-9999-4999-8999-999999999993";
        const auto source = directory.filePath("upload.png");
        const auto png = avatarProbePng(false);
        QCOMPARE(png.size(), qsizetype(230));
        QCOMPARE(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex(),
            QByteArray("9fb39a2b2737f88bdf0b383c4fcea4b0f3636e584b280f10afd18444739ed1f9"));
        const auto expected = QImage::fromData(png, "PNG");
        QVERIFY(!expected.isNull()); QCOMPARE(expected.size(), QSize(96, 64));
        {
            QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(png), qint64(png.size()));
        }
        auto metadata = avatarProbeMetadata(fileId, png);
        metadata["stream_uuid"] = streamId; metadata["name"] = "upload.png";
        const QString caption = "Caption before upload";
        QString uploadedDraft;
        QJsonObject postedRequest, canonical;
        int uploads = 0, messagePosts = 0, canonicalGets = 0, historyGets = 0;
        int metadataGets = 0, binaryGets = 0, fileWrites = 0;
        bool uploadHeadersOkay = true, fileHeadersOkay = true, postContractOkay = true;
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const QString filePath = "/api/workspace/v1/messenger/files/" + fileId;
            if (path == "/api/workspace/v1/messenger/files/" && request.method == "POST") {
                ++uploads;
                uploadHeadersOkay = uploadHeadersOkay
                    && request.headers.value("authorization") == QByteArray("Bearer synthetic-project-token")
                    && request.headers.value("cookie").isEmpty()
                    && request.headers.value("content-type").startsWith("multipart/form-data;")
                    && request.headers.value("content-length").toLongLong() == request.rawBody.size();
                const auto contentType = request.headers.value("content-type");
                auto boundary = contentType.mid(contentType.indexOf("boundary=") + 9).trimmed();
                if (boundary.startsWith('"') && boundary.endsWith('"')) boundary = boundary.mid(1, boundary.size() - 2);
                const auto fileAt = request.rawBody.indexOf("filename=\"upload.png\"");
                const auto headerEnd = request.rawBody.indexOf("\r\n\r\n", fileAt);
                const auto bodyEnd = request.rawBody.indexOf("\r\n--" + boundary, headerEnd + 4);
                const auto uploadedBytes = request.rawBody.mid(headerEnd + 4, bodyEnd - headerEnd - 4);
                uploadHeadersOkay = uploadHeadersOkay && contentType.contains("boundary=") && !boundary.isEmpty()
                    && fileAt >= 0 && headerEnd > fileAt && bodyEnd > headerEnd
                    && request.rawBody.count("filename=\"upload.png\"") == 1
                    && request.rawBody.left(headerEnd).contains("name=\"file\"")
                    && request.rawBody.left(headerEnd).toLower().contains("content-type: image/png")
                    && uploadedBytes == png
                    && QCryptographicHash::hash(uploadedBytes, QCryptographicHash::Sha256).toHex()
                        == QByteArray("9fb39a2b2737f88bdf0b383c4fcea4b0f3636e584b280f10afd18444739ed1f9")
                    && request.rawBody.contains("name=\"stream_uuid\"\r\n\r\n" + streamId.toUtf8() + "\r\n")
                    && request.rawBody.contains("name=\"name\"\r\n\r\nupload.png\r\n");
                qInfo().noquote() << "UPLOAD_MULTIPART_OBSERVER" << QJsonDocument(QJsonObject{
                    {"contentType", QString::fromLatin1(contentType)}, {"bytes", request.rawBody.size()},
                    {"fileAt", fileAt}, {"headerEnd", headerEnd}, {"bodyEnd", bodyEnd},
                    {"fileHeaders", QString::fromLatin1(request.rawBody.left(headerEnd))},
                    {"selectedPngEqual", uploadedBytes == png}, {"contractOkay", uploadHeadersOkay}
                }).toJson(QJsonDocument::Compact);
                // The complete multipart body contains the exact selected PNG
                // and intended stream; no raw credential-bearing headers are logged.
                return Response{metadata, 201};
            }
            if (path.startsWith("/api/workspace/v1/messenger/files/")) {
                fileHeadersOkay = fileHeadersOkay
                    && request.headers.value("authorization") == QByteArray("Bearer synthetic-project-token")
                    && request.headers.value("cookie").isEmpty();
                if (request.method != "GET") { ++fileWrites; return Response{QJsonObject{}, 405}; }
                if (path == filePath) { ++metadataGets; return Response{metadata}; }
                if (path == filePath + "/actions/download") {
                    ++binaryGets; Response result; result.rawBody = png; result.rawContentType = "image/png";
                    return result;
                }
                return Response{QJsonObject{}, 404};
            }
            if (path == "/api/workspace/v1/messenger/messages/" && request.method == "POST") {
                ++messagePosts; postedRequest = request.body;
                const auto payload = request.body.value("payload").toObject();
                const auto id = request.body.value("uuid").toString();
                postContractOkay = postContractOkay && !QUuid(id).isNull()
                    && request.body.value("stream_uuid").toString() == streamId
                    && request.body.value("topic_uuid").toString() == topicId
                    && payload.value("kind").toString() == "markdown"
                    && payload.value("content").toString() == uploadedDraft;
                if (!postContractOkay) return Response{QJsonObject{}, 400};
                canonical = message(payload.value("content").toString(), id);
                canonical["user_uuid"] = userId; canonical["author_uuid"] = userId; canonical["is_own"] = true;
                canonical["pinned"] = false; canonical["reactions"] = QJsonObject{};
                canonical["reaction_users"] = QJsonObject{}; canonical["updated_at"] = canonical.value("created_at");
                return Response{canonical, 201};
            }
            if (!canonical.isEmpty() && path == "/api/workspace/v1/messenger/messages/" + canonical.value("uuid").toString()
                && request.method == "GET") { ++canonicalGets; return Response{canonical}; }
            if (path == "/api/workspace/v1/messenger/messages/" && request.method == "GET") {
                ++historyGets;
                const QUrlQuery query(request.target);
                return query.queryItemValue("stream_uuid") == streamId && query.queryItemValue("topic_uuid") == topicId
                    && !canonical.isEmpty() ? Response{QJsonArray{canonical}} : Response{QJsonArray{}};
            }
            return fallback(request);
        };
        ApiClient api(nullptr, true); Session session(&api); FileClient files(&api);
        MessageImages images(&api, &session, &files);
        NativeController controller(&api, &session); controller.useFiles(&files);
        NativePreferences preferences(directory.filePath("upload-caption-image.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences, nullptr, nullptr, &images);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Uploaded image window did not settle before engine teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900); QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        QCOMPARE(window->devicePixelRatio(), qreal(1)); QCOMPARE(window->size(), QSize(1440, 900));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); QTRY_VERIFY(controller.channelMembersVerified());
        preferences.setOwner(controller.ownerKey()); preferences.setLocale("en"); preferences.setMode("light");
        QTRY_VERIFY(visibleItem(window->contentItem(), "stream-" + streamId));
        click(window, visibleItem(window->contentItem(), "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        QTRY_VERIFY(visibleItem(window->contentItem(), "topic-" + topicId));
        click(window, visibleItem(window->contentItem(), "topic-" + topicId));
        QTRY_COMPARE(controller.selectedStream(), streamId); QTRY_COMPARE(controller.selectedTopic(), topicId);
        QTRY_VERIFY(!controller.busy()); QTRY_VERIFY(historyGets > 0); QVERIFY(controller.messages().isEmpty());
        QVERIFY(controller.canAttach());
        QTRY_VERIFY(visibleItem(window->contentItem(), "messageComposer"));
        click(window, visibleItem(window->contentItem(), "messageComposer"));
        for (const auto key : caption.toLatin1()) QTest::keyClick(window, key);
        QTRY_COMPARE(controller.draft(), caption);
        auto *picker = window->findChild<QObject *>("attachmentPicker"); QVERIFY(picker);
        QVERIFY(picker->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        QSignalSpy accepted(picker, SIGNAL(accepted())); QVERIFY(accepted.isValid());
        click(window, visibleItem(window->contentItem(), "attachButton"));
        QTRY_VERIFY(picker->property("visible").toBool());
        QCOMPARE(window->property("pickerOwner").toString(), controller.ownerKey());
        QCOMPARE(window->property("pickerStream").toString(), streamId);
        QCOMPARE(window->property("pickerTopic").toString(), topicId);
        QTRY_VERIFY(fileEntry(QUrl::fromLocalFile(source)));
        auto *entry = fileEntry(QUrl::fromLocalFile(source)); QVERIFY(entry && entry->window());
        const auto filePoint = entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2)).toPoint();
        QVERIFY(QRect(QPoint{}, entry->window()->size()).contains(filePoint));
        QTest::mouseDClick(entry->window(), Qt::LeftButton, {}, filePoint);
        QTRY_COMPARE(accepted.size(), 1); QTRY_COMPARE(uploads, 1); QVERIFY(uploadHeadersOkay);
        QTRY_VERIFY(!controller.fileBusy());
        QTRY_VERIFY(controller.draft().contains("![upload.png](") && controller.draft().contains("urn:image:" + fileId));
        QVERIFY(controller.draft().contains(caption)); QVERIFY(controller.error().isEmpty());
        uploadedDraft = controller.draft();
        QTRY_VERIFY(visibleItem(window->contentItem(), "messageComposer"));
        QCOMPARE(visibleItem(window->contentItem(), "messageComposer")->property("text").toString(), uploadedDraft);
        QVERIFY(controller.canSend());
        QTRY_VERIFY(visibleItem(window->contentItem(), "sendButton")
            && visibleItem(window->contentItem(), "sendButton")->isEnabled());
        click(window, visibleItem(window->contentItem(), "sendButton"));
        QTRY_COMPARE(messagePosts, 1); QTRY_COMPARE(canonicalGets, 1); QVERIFY(postContractOkay);
        QCOMPARE(postedRequest.value("payload").toObject().value("content").toString(), uploadedDraft);
        QCOMPARE(canonical.value("payload").toObject().value("content").toString(), uploadedDraft);
        QTRY_VERIFY(!controller.busy()); QTRY_VERIFY(controller.draft().isEmpty());
        const auto canonicalLoaded = [&] {
            const auto rows = controller.messages();
            if (canonical.isEmpty() || rows.size() != 1) return false;
            const auto row = rows.first().toMap();
            return row.value("id").toString() == canonical.value("uuid").toString()
                && row.value("content").toString() == uploadedDraft
                && row.value("authorId").toString() == userId && row.value("own").toBool();
        };
        QTRY_VERIFY(canonicalLoaded());
        QTRY_VERIFY(visibleItem(window->contentItem(), "messageComposer"));
        QCOMPARE(visibleItem(window->contentItem(), "messageComposer")->property("text").toString(), QString{});
        const auto ownId = canonical.value("uuid").toString();
        QTRY_VERIFY(visibleItem(window->contentItem(), "message-content-" + ownId));
        auto *viewport = item(window->contentItem(), "messageList"); QVERIFY(viewport);
        QTRY_VERIFY(!viewport->property("positioning").toBool());
        QSignalSpy frame(window, &QQuickWindow::frameSwapped); QVERIFY(frame.isValid());
        window->update(); QTRY_VERIFY(!frame.isEmpty());
        QRect paintedImage, sampledBubble; int matchingPixels = 0;
        const QVector<QPoint> probes{{8, 8}, {24, 16}, {72, 16}, {24, 48}, {72, 48}, {88, 56}};
        const auto closeColor = [](const QColor &a, const QColor &b) {
            return qAbs(a.red() - b.red()) <= 3 && qAbs(a.green() - b.green()) <= 3 && qAbs(a.blue() - b.blue()) <= 3;
        };
        const auto pixelsPainted = [&] {
            paintedImage = {}; sampledBubble = {}; matchingPixels = 0;
            auto *bubble = visibleItem(window->contentItem(), "message-" + ownId);
            if (!bubble || bubble->window() != window) return false;
            const auto pixels = window->grabWindow(); if (pixels.isNull()) return false;
            sampledBubble = QRectF(bubble->mapToScene(QPointF{}), bubble->size()).toAlignedRect();
            if (!pixels.rect().contains(sampledBubble)) return false;
            for (int y = sampledBubble.top(); y <= sampledBubble.bottom(); ++y)
                for (int x = sampledBubble.left(); x <= sampledBubble.right(); ++x) {
                    const auto color = pixels.pixelColor(x, y); bool known = false;
                    for (const auto &point : probes) if (closeColor(color, expected.pixelColor(point))) { known = true; break; }
                    if (known) { paintedImage |= QRect(x, y, 1, 1); ++matchingPixels; }
                }
            if (qAbs(paintedImage.width() - 96) > 1 || qAbs(paintedImage.height() - 64) > 1 || matchingPixels < 5500) return false;
            for (const auto &point : probes) {
                const QPoint at(paintedImage.left() + qFloor((point.x() + .5) * paintedImage.width() / 96),
                    paintedImage.top() + qFloor((point.y() + .5) * paintedImage.height() / 64));
                if (!closeColor(pixels.pixelColor(at), expected.pixelColor(point))) return false;
            }
            return true;
        };
        const auto evidence = [&](const char *phase) {
            qInfo().noquote() << "UPLOADED_OWN_IMAGE" << QJsonDocument(QJsonObject{
                {"phase", QString::fromLatin1(phase)}, {"uploadAccepted", uploads == 1 && uploadHeadersOkay},
                {"messagePosts", messagePosts}, {"canonicalGets", canonicalGets}, {"canonicalLoaded", canonicalLoaded()},
                {"draftCleared", controller.draft().isEmpty()}, {"metadataGets", metadataGets}, {"binaryGets", binaryGets},
                {"matchingPixels", matchingPixels}, {"imagePixels", QJsonArray{paintedImage.x(), paintedImage.y(), paintedImage.width(), paintedImage.height()}},
                {"uploadedDraft", uploadedDraft}
            }).toJson(QJsonDocument::Compact);
        };
        pixelsPainted(); evidence("before-pixel-oracle");
        auto failureEvidence = qScopeGuard([&] { if (QTest::currentTestFailed()) evidence("failed-case"); });
        // Upload, canonical message admission, own-row projection and draft
        // clearing are all positive before the first intended product failure.
        QTRY_VERIFY2(pixelsPainted(), "The PNG selected with its caption must paint in the canonical own message after successful Send");
        QCOMPARE(uploads, 1); QCOMPARE(messagePosts, 1); QCOMPARE(canonicalGets, 1);
        QCOMPARE(metadataGets, 1); QCOMPARE(binaryGets, 1); QVERIFY(fileHeadersOkay);
        QCOMPARE(fileWrites, 0); QCOMPARE(fixture.posts + fixture.memberPosts, 0);
        QVERIFY(canonicalLoaded()); QVERIFY(controller.draft().isEmpty()); evidence("own-image-painted");
        const auto captureDirectory = qEnvironmentVariable("CASSI_NATIVE_E2EE_SCREENSHOT_DIRECTORY");
        if (!captureDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDirectory));
            QVERIFY(window->grabWindow().save(captureDirectory + "/uploaded-caption-image-en-light.png"));
            QFile dto(captureDirectory + "/uploaded-caption-image-handler-dto.json");
            QVERIFY(dto.open(QIODevice::WriteOnly));
            const auto bytes = QJsonDocument(canonical).toJson(QJsonDocument::Indented);
            QCOMPARE(dto.write(bytes), qint64(bytes.size()));
        }
        const bool closed = closeIncubatedTestWindow(engine, window);
        if (closed) closeBeforeEngine.dismiss(); QVERIFY(closed);
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

    void loginCardMountsOrganizationStepOnly()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        preferences.setLocale("en");
        MockServer server;
        server.handler = [](const Request &request) {
            if (request.target.path().endsWith("/server_settings"))
                return Response{QJsonObject{{"result", "success"}, {"msg", ""},
                    {"realm_name", "Acme Realm"}, {"registration_url", "https://reg.example/signup"}}};
            return sessionResponse(request);
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
        QCOMPARE(item(content, "loginTitle")->property("text").toString(), QString("Connect to server"));
        QCOMPARE(item(content, "loginDescription")->property("text").toString(),
                 QString("Enter your organization address first."));
        // Reference test 154: only the organization field mounts on the first step.
        QVERIFY(item(content, "serverInput"));
        QVERIFY(!item(content, "userInput"));
        QVERIFY(!item(content, "passwordInput"));
        QVERIFY(!item(content, "signInButton"));
        QVERIFY(!item(content, "realmPreviewCard"));
        QVERIFY(!item(content, "defaultOrganizationBlock"));
        QVERIFY(!item(content, "organizationNextButton")->isEnabled());
        item(content, "serverInput")->setProperty("text", server.origin().toString());
        QTRY_VERIFY(item(content, "organizationNextButton")->isEnabled());
        click(window, item(content, "organizationNextButton"));
        QTRY_VERIFY(item(content, "userInput"));
        QVERIFY(item(content, "realmPreviewCard"));
        QCOMPARE(item(content, "realmPreviewName")->property("text").toString(), QString("Acme Realm"));
        // Without a realm icon in the payload the card keeps the built-in glyph.
        QCOMPARE(item(content, "realmPreviewIcon")->property("source").toString(),
                 QString("qrc:/workspace/brand/organization-fallback.svg"));
        QVERIFY(!item(content, "organizationNextButton"));
        QCOMPARE(item(content, "loginDescription")->property("text").toString(),
                 QString("Choose a sign-in method or enter your email/login and password."));
        // The back affordance returns to the organization step with the field text kept.
        click(window, item(content, "loginBackButton"));
        QTRY_VERIFY(item(content, "organizationNextButton"));
        QVERIFY(!item(content, "userInput"));
        QCOMPARE(item(content, "serverInput")->property("text").toString(), server.origin().toString());
        window->close();
    }

    void loginCardRealmBlurStartsDiscovery()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        preferences.setLocale("en");
        MockServer server;
        server.handler = [](const Request &request) {
            if (request.target.path().endsWith("/server_settings"))
                return Response{QJsonObject{{"result", "success"}, {"msg", ""},
                    {"realm_name", "Acme Realm"}, {"registration_url", "https://reg.example/signup"}}};
            return sessionResponse(request);
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
        // Reference handleRealmBlur: leaving a valid address starts discovery
        // without advancing the wizard.
        click(window, item(content, "serverInput"));
        item(content, "serverInput")->setProperty("text", server.origin().toString());
        QTRY_VERIFY(item(content, "organizationNextButton")->isEnabled());
        QTest::keyClick(window, Qt::Key_Tab); // focus moves off the field
        QTRY_VERIFY(item(content, "realmPreviewCard"));
        QVERIFY(!item(content, "userInput"));
        window->close();
    }

    void loginCardDiscoveryFailureKeepsCredentialsHidden()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        preferences.setLocale("en");
        MockServer server;
        server.handler = [](const Request &request) {
            if (request.target.path().endsWith("/server_settings"))
                return Response{QJsonObject{}, 503};
            return sessionResponse(request);
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
        item(content, "serverInput")->setProperty("text", server.origin().toString());
        QTRY_VERIFY(item(content, "organizationNextButton")->isEnabled());
        click(window, item(content, "organizationNextButton"));
        QTRY_VERIFY(!controller.error().isEmpty());
        QVERIFY(visibleItem("organizationErrorBanner"));
        // Reference test 470: discovery failure keeps the credentials hidden.
        QVERIFY(!item(content, "userInput"));
        QCOMPARE(controller.error(),
                 QString("Could not load organization settings. Check the server address and try again."));
        click(window, item(content, "organizationNextButton"));
        QVERIFY(!item(content, "userInput")); // no discovery success means no reveal
        window->close();
    }

    void loginCardOffersPersistedOrganizationHistory()
    {
        QTemporaryDir preferencesDirectory;
        NativePreferences preferences(preferencesDirectory.filePath("preferences.ini"));
        preferences.setLocale("en");
        MockServer server;
        server.handler = [](const Request &request) {
            if (request.target.path().endsWith("/server_settings"))
                return Response{QJsonObject{{"result", "success"}, {"msg", ""},
                    {"realm_name", "Acme Realm"}, {"registration_url", "https://reg.example/signup"}}};
            return sessionResponse(request);
        };
        preferences.recordOrganizationOrigin(server.origin().toString());
        ApiClient api(nullptr, true);
        Session session(&api);
        NativeController controller(&api, &session);
        controller.setPreferences(&preferences);
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *content = window->contentItem();
        // Reference tests 162 and 180: persisted origins are suggested after a
        // remount and choosing one fills the field without submitting the form.
        click(window, item(content, "serverInput"));
        QTRY_VERIFY(item(content, "organizationHistoryOption-0"));
        click(window, item(content, "organizationHistoryOption-0"));
        QCOMPARE(item(content, "serverInput")->property("text").toString(), server.origin().toString());
        QVERIFY(!item(content, "userInput")); // choosing an entry never submits the form
        click(window, item(content, "organizationNextButton"));
        QTRY_VERIFY(item(content, "userInput"));
        window->close();
    }
    void upToEditLastAndSaveUseTheComposerEditMode()
    {
        QTest::failOnWarning();
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        const auto older = message("Older own message", messageId);
        auto newer = message("Newest own message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        int puts = 0;
        fixture.server.handler = [&, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") { ++puts; newer["payload"] = request.body.value("payload"); return {newer}; }
            if (request.method == "GET" && path.endsWith("/" + returnedId)) return {newer};
            if (request.method == "GET" && path.endsWith("/" + messageId)) return {older};
            return fallback(request);
        };
        QTemporaryDir directory;
        NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        controller.useDrafts(&drafts);
        NativePreferences preferences(directory.filePath("appearance.ini")); preferences.setLocale("en");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data())) QTest::qFail("Inline edit teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "cassi-native-edit", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(!controller.busy() && !controller.streams().isEmpty());
        controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1);
        if (controller.selectedTopic() != topicId) controller.selectTopic(topicId);
        QTRY_COMPARE(controller.messages().size(), 2);
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        auto *content = window->contentItem();
        auto *composer = item(content, "messageComposer"); QVERIFY(composer);
        auto *shell = item(content, "messageComposerShell"); QVERIFY(shell);
        composer->forceActiveFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(composer->hasActiveFocus());
        const auto restingHeight = shell->height();

        // Shift+ArrowUp is a text selection, not the Source hotkey (hotkeys.lib.ts matcher).
        QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
        QVERIFY(controller.editingMessageId().isEmpty());

        // Up-to-edit-last on the empty composer edits the newest own message.
        QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(controller.editingMessageId(), returnedId);
        QCOMPARE(controller.draft(), QString("Newest own message"));
        auto *notice = item(content, "editMessageNotice"); QVERIFY(notice);
        QTRY_VERIFY(notice->isVisible());
        QCOMPARE(notice->height(), 33.0);
        QCOMPARE(item(content, "editMessageNoticeText")->property("font").value<QFont>().pixelSize(), 14);
        QTRY_COMPARE(shell->height(), restingHeight + 33.0);
        auto *saveEdit = window->findChild<QObject *>("saveEditButton"); QVERIFY(saveEdit);
        QTRY_VERIFY(saveEdit->property("enabled").toBool());
        // Escape leaves the mode; the composer keeps the draft that was there before.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!notice->isVisible());
        QTRY_VERIFY(controller.editingMessageId().isEmpty());
        QTRY_COMPARE(shell->height(), restingHeight);

        // A draft typed before editing survives the whole session (Source snapshot/restore).
        QInputMethodEvent typed; typed.setCommitString("note typed before editing");
        QCoreApplication::sendEvent(composer, &typed);
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("note typed before editing"));
        const auto typedHeight = shell->height();
        auto *bubble = item(content, "message-" + returnedId); QVERIFY(bubble);
        QTest::mouseClick(window, Qt::RightButton, {},
            bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
        auto *editAction = qobject_cast<QQuickItem *>(window->findChild<QObject *>("editMessageAction")); QVERIFY(editAction);
        QTRY_VERIFY(editAction->isEnabled());
        click(window, editAction);
        QTRY_VERIFY(notice->isVisible());
        QTRY_COMPARE(shell->height(), typedHeight + 33.0);
        QTRY_COMPARE(composer->property("text").toString(), QString("Newest own message"));
        QTest::qWait(200);
        QCOMPARE(drafts.text(streamId, topicId), QString("note typed before editing"));
        QVERIFY(saveEdit->property("enabled").toBool());

        // Enter saves the edit instead of sending a new message.
        composer->forceActiveFocus(Qt::OtherFocusReason);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(puts, 1);
        QTRY_VERIFY(controller.editingMessageId().isEmpty());
        QTRY_VERIFY(!notice->isVisible());
        QTRY_COMPARE(shell->height(), typedHeight);
        QCOMPARE(drafts.text(streamId, topicId), QString("note typed before editing"));
        QTRY_COMPARE(controller.draft(), QString("note typed before editing"));
        QTRY_COMPARE(composer->property("text").toString(), QString("note typed before editing"));
        QVERIFY(controller.error().isEmpty());
    }

    // Reply banner + quote jump, measured against workspace_ui 61dacef:
    // composer preview chrome (message-composer-reply-quote-preview.ui.tsx and
    // message-composer-preface.ui.tsx:222-262): 2 px accent left bar (#7087ff),
    // 12 px/16 px weight-500 accent header "Reply: <name>", 14 px/20 px body,
    // frame padding 8 x / 6 y; bubble quote frame (workspace-message-quote.ui.tsx):
    // same accent bar, radius 6, vertical margin 4 px.
    void replyBannerAndQuoteJumpUseTheComposer()
    {
        QTest::failOnWarning();
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        auto origin = message("Peer origin", messageId);
        origin["is_own"] = false;
        const QString quotedId = "5a5a5a5a-5a5a-4a5a-8a5a-5a5a5a5a5a5a";
        const auto quoted = message(QString("[Peer](urn:quote:%1)\n\nanswer body").arg(messageId), quotedId);
        const QString quoteOnlyId = "3c3c3c3c-3c3c-4c3c-8c3c-3c3c3c3c3c3c";
        const auto quoteOnly = message(QString("[Peer](urn:quote:%1)").arg(messageId), quoteOnlyId);
        QJsonObject posted; int posts = 0;
        fixture.server.handler = [&, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path.endsWith("/messages/")) {
                ++posts; posted = request.body;
                return {message(posted.value("payload").toObject().value("content").toString(), returnedId), 201};
            }
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{origin, quoted, quoteOnly}};
            if (request.method == "GET" && path.endsWith("/" + returnedId))
                return {message(posted.value("payload").toObject().value("content").toString(), returnedId)};
            return fallback(request);
        };
        QTemporaryDir directory;
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        NativePreferences preferences(directory.filePath("appearance.ini")); preferences.setLocale("en");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        const auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data())) QTest::qFail("Reply teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "cassi-native-reply", "synthetic-password");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_VERIFY(!controller.busy() && !controller.streams().isEmpty());
        controller.selectStream(streamId);
        QTRY_COMPARE(controller.topics().size(), 1);
        if (controller.selectedTopic() != topicId) controller.selectTopic(topicId);
        QTRY_COMPARE(controller.messages().size(), 3);
        auto *content = window->contentItem();
        auto *composer = item(content, "messageComposer"); QVERIFY(composer);
        auto *shell = item(content, "messageComposerShell"); QVERIFY(shell);
        const auto restingHeight = shell->height();

        // The reply entry opens the banner over the composer.
        auto *bubble = item(content, "message-" + messageId); QVERIFY(bubble);
        QTest::mouseClick(window, Qt::RightButton, {},
            bubble->mapToScene(QPointF(bubble->width() / 2, bubble->height() / 2)).toPoint());
        auto *replyAction = qobject_cast<QQuickItem *>(window->findChild<QObject *>("replyMessageAction")); QVERIFY(replyAction);
        click(window, replyAction);
        auto *notice = item(content, "replyNotice"); QVERIFY(notice);
        QTRY_VERIFY(notice->isVisible());
        QTRY_COMPARE(shell->height(), restingHeight + notice->height());
        auto *header = item(content, "replyAuthorText"); QVERIFY(header);
        QVERIFY2(header->property("text").toString().startsWith("Reply: "),
                 qPrintable(header->property("text").toString()));
        QCOMPARE(header->property("font").value<QFont>().pixelSize(), 12);
        QCOMPARE(header->property("lineHeight").toReal(), 16.0);
        auto *bar = item(content, "replyAccentBar"); QVERIFY(bar);
        QCOMPARE(bar->property("width").toReal(), 2.0);
        auto *excerpt = item(content, "replyExcerptText"); QVERIFY(excerpt);
        QCOMPARE(excerpt->property("text").toString(), QString("Peer origin"));
        QCOMPARE(excerpt->property("font").value<QFont>().pixelSize(), 14);
        QCOMPARE(excerpt->property("maximumLineCount").toInt(), 2);

        // Cancel closes the banner without sending anything.
        auto *cancel = item(content, "cancelReplyButton"); QVERIFY(cancel);
        click(window, qobject_cast<QQuickItem *>(cancel));
        QTRY_VERIFY(!notice->isVisible());
        QTRY_COMPARE(shell->height(), restingHeight);
        QCOMPARE(posts, 0);

        // Typing an answer and sending posts the composed urn:quote line.
        composer->forceActiveFocus(Qt::OtherFocusReason);
        QVERIFY(controller.beginReply(messageId));
        QTRY_VERIFY(notice->isVisible());
        QInputMethodEvent typed; typed.setCommitString("answer typed");
        QCoreApplication::sendEvent(composer, &typed);
        auto *sendButton = window->findChild<QObject *>("sendButton"); QVERIFY(sendButton);
        QTRY_VERIFY(sendButton->property("enabled").toBool());
        click(window, qobject_cast<QQuickItem *>(sendButton));
        QTRY_COMPARE(posts, 1);
        const auto wire = posted.value("payload").toObject().value("content").toString();
        QVERIFY2(wire.contains("urn:quote:" + messageId), qPrintable(wire));
        QVERIFY2(wire.endsWith("\n\nanswer typed"), qPrintable(wire));
        QTRY_VERIFY(controller.replyMessageId().isEmpty());
        QTRY_VERIFY(!notice->isVisible());

        // Escape leaves an open reply session while the composer has focus.
        QVERIFY(controller.beginReply(messageId));
        QTRY_VERIFY(notice->isVisible());
        composer->forceActiveFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(composer->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(controller.replyMessageId().isEmpty());

        // The received message renders the quote frame and jumps to the origin.
        auto *quote = item(content, "message-quote-" + quotedId); QVERIFY(quote);
        QTRY_VERIFY(quote->isVisible());
        auto *quoteBar = item(content, "quoteAccentBar-" + quotedId); QVERIFY(quoteBar);
        QCOMPARE(quoteBar->property("width").toReal(), 2.0);
        click(window, qobject_cast<QQuickItem *>(quote));
        QTRY_COMPARE_WITH_TIMEOUT(controller.anchorMessageId(), messageId, 3000);
        auto *highlight = item(content, "messageHighlight-" + messageId);
        QTRY_VERIFY(highlight && highlight->isVisible());

        // A quote-only message shows the frame alone: the body never re-renders
        // the raw anchor the frame already consumed.
        auto *bareBody = item(content, "message-content-" + quoteOnlyId); QVERIFY(bareBody);
        QVERIFY2(!bareBody->property("text").toString().contains("urn:quote"),
                 qPrintable(bareBody->property("text").toString()));
        QTRY_VERIFY(item(content, "message-quote-" + quoteOnlyId)->isVisible());
    }

    // --- media viewer (issue #8, docs/NATIVE_DESKTOP_MEDIA_VIEWER.md) ---
    // Geometry assertions cite the measured reference at workspace_ui
    // 61dacef78dab2f5d0be76f283bebf49fe29b84af through the design-doc table.

    void mediaViewerOpensNavigatesAndClosesLikeTheReference()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString fileA = QStringLiteral("9f000000-0000-4000-8000-00000000000a");
        const QString fileB = QStringLiteral("9f000000-0000-4000-8000-00000000000b");
        const QString first = QStringLiteral("9e000000-0000-4000-8000-000000000001");
        const QString second = QStringLiteral("9e000000-0000-4000-8000-000000000002");
        const QByteArray png = avatarProbePng(false);
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            for (const auto &fileUuid : {fileA, fileB}) {
                const auto file = "/api/workspace/v1/messenger/files/" + fileUuid;
                if (path.startsWith(file)) {
                    if (path == file) {
                        auto metadata = avatarProbeMetadata(fileUuid, png);
                        metadata["stream_uuid"] = streamId; metadata["name"] = "shot.png";
                        return Response{metadata};
                    }
                    if (path == file + "/actions/download") {
                        Response result; result.rawBody = png; result.rawContentType = "image/png";
                        return result;
                    }
                    return Response{QJsonObject{}, 404};
                }
            }
            if (path.endsWith("/messages/") && request.method == "GET") {
                ++fixture.historyGets;
                return Response{QJsonArray{mediaViewerRow(first, fileA, png.size()),
                                           mediaViewerRow(second, fileB, png.size())}};
            }
            return fallback(request);
        };
        ApiClient api(nullptr, true); Session session(&api); FileClient files(&api);
        MessageImages images(&api, &session, &files);
        NativeController controller(&api, &session); controller.useFiles(&files);
        NativePreferences preferences(directory.filePath("media-viewer.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences, nullptr, nullptr, &images);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Media viewer window did not settle before engine teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900); QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); QTRY_VERIFY(controller.channelMembersVerified());
        auto *content = window->contentItem();
        click(window, visibleItem(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, visibleItem(content, "topic-" + topicId));
        QTRY_COMPARE(controller.selectedTopic(), topicId);
        QTRY_COMPARE(controller.messages().size(), 2);
        QTRY_VERIFY(visibleItem(content, "inline-image-" + first));
        QTRY_VERIFY(visibleItem(content, "inline-image-" + second));
        auto *viewer = controller.mediaViewer();
        QVERIFY(viewer && !viewer->open());
        QVERIFY(!item(content, "mediaViewer-overlay")->isVisible());

        click(window, visibleItem(content, "inline-image-" + first));
        QTRY_VERIFY(viewer->open());
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *shot = visibleItem(content, "mediaViewer-image"); return shot && shot->isVisible(); })(), 4000);
        QCOMPARE(visibleItem(content, "mediaViewer-counter")->property("text").toString(), QString("1 / 2"));

        // 48x48 nav circles 16 px from the edges, vertically centered
        // (media-viewer-controls.ui.tsx:6-7); 40x40 toolbar buttons top-right
        // 16 px (media-viewer-toolbar.ui.tsx:7-8,52); strip anchored to the
        // bottom with 56x56 thumbnails (media-viewer-thumbnails.ui.tsx:78-90).
        auto *prev = visibleItem(content, "mediaViewer-prev");
        auto *next = visibleItem(content, "mediaViewer-next");
        QCOMPARE(prev->width(), 48.0); QCOMPARE(prev->height(), 48.0);
        QCOMPARE(prev->mapToItem(content, QPointF(0, 0)).x(), 16.0);
        QCOMPARE(prev->mapToItem(content, QPointF(0, 0)).y(), content->height() / 2 - 24.0);
        QCOMPARE(next->mapToItem(content, QPointF(0, 0)).x(), content->width() - 16.0 - 48.0);
        auto *closeButton = visibleItem(content, "mediaViewer-close");
        QCOMPARE(closeButton->width(), 40.0); QCOMPARE(closeButton->height(), 40.0);
        QCOMPARE(closeButton->mapToItem(content, QPointF(0, 0)).x(), content->width() - 16.0 - 40.0);
        QCOMPARE(closeButton->mapToItem(content, QPointF(0, 0)).y(), 16.0);
        auto *strip = visibleItem(content, "mediaViewer-strip"); QVERIFY(strip);
        QCOMPARE(strip->y() + strip->height(), content->height());
        auto *thumb = visibleItem(content, "mediaViewer-thumb-1"); QVERIFY(thumb);
        QCOMPARE(thumb->width(), 56.0); QCOMPARE(thumb->height(), 56.0);

        // Review reproduction (exo-review finding=5): each arrow must expose its
        // own accessible name; a previous arrow bound to the wrong property
        // reports the wrong label for the previous arrow. QQuickAccessibleAttached
        // is a private header, so the attached object is read as the item's only
        // non-item child that carries a "name" property.
        const auto accessibleName = [](QQuickItem *target) {
            for (auto *child : target->children())
                if (!child->inherits("QQuickItem") && child->metaObject()->indexOfProperty("name") != -1)
                    return child->property("name").toString();
            return QString{};
        };
        QCOMPARE(accessibleName(prev), QString("Previous"));
        QCOMPARE(accessibleName(next), QString("Next"));

        // Review reproduction (exo-review finding=4): the image element's box
        // equals its painted image in the reference (object-contain img), so a
        // click inside the item bounds but outside the painted pixels (the
        // 96x64 fixture paints 1062x708, leaving pillarbox bands at x in
        // [64,189] and [1251,1376]) is a backdrop click and closes the viewer.
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(100, 450));
        QTRY_VERIFY(!viewer->open());
        click(window, visibleItem(content, "inline-image-" + first));
        QTRY_VERIFY(viewer->open());

        // Click and keyboard navigation clamps without wrap-around.
        click(window, next);
        QCOMPARE(viewer->index(), 1);
        QCOMPARE(visibleItem(content, "mediaViewer-counter")->property("text").toString(), QString("2 / 2"));
        click(window, next);
        QCOMPARE(viewer->index(), 1);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(viewer->index(), 0);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(viewer->index(), 1);
        // Escape closes (media-viewer-backdrop.ui.tsx:27-51).
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!viewer->open());
        QVERIFY(!item(content, "mediaViewer-overlay")->isVisible());

        // A thumbnail navigates; a backdrop click closes the overlay.
        click(window, visibleItem(content, "inline-image-" + second));
        QTRY_VERIFY(viewer->open());
        QCOMPARE(viewer->index(), 1);
        QTRY_VERIFY(visibleItem(content, "mediaViewer-thumb-0"));
        click(window, visibleItem(content, "mediaViewer-thumb-0"));
        QCOMPARE(viewer->index(), 0);
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(8, 300));
        QTRY_VERIFY(!viewer->open());
    }

    void mediaViewerBackdropClickClosesWhileSlotLoadsAndStaysOpenWhenReady()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString fileA = QStringLiteral("9f000000-0000-4000-8000-00000000000a");
        const QString first = QStringLiteral("9e000000-0000-4000-8000-000000000001");
        const QByteArray png = avatarProbePng(false);
        bool holdFirstDownload = true;
        std::function<void()> releaseDownload;
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto file = "/api/workspace/v1/messenger/files/" + fileA;
            if (path.startsWith(file)) {
                if (path == file) {
                    auto metadata = avatarProbeMetadata(fileA, png);
                    metadata["stream_uuid"] = streamId; metadata["name"] = "shot.png";
                    return Response{metadata};
                }
                if (path == file + "/actions/download") {
                    Response result; result.rawBody = png; result.rawContentType = "image/png";
                    if (holdFirstDownload) {
                        holdFirstDownload = false;
                        result.defer = [&](std::function<void()> send) { releaseDownload = std::move(send); };
                    }
                    return result;
                }
                return Response{QJsonObject{}, 404};
            }
            if (path.endsWith("/messages/") && request.method == "GET") {
                ++fixture.historyGets;
                return Response{QJsonArray{mediaViewerRow(first, fileA, png.size())}};
            }
            return fallback(request);
        };
        ApiClient api(nullptr, true); Session session(&api); FileClient files(&api);
        MessageImages images(&api, &session, &files);
        NativeController controller(&api, &session); controller.useFiles(&files);
        NativePreferences preferences(directory.filePath("loading-slot.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences, nullptr, nullptr, &images);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, &releaseDownload, guard = QPointer<QQuickWindow>(window)] {
            if (releaseDownload) releaseDownload();
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Loading-slot viewer window did not settle before engine teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900); QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); QTRY_VERIFY(controller.channelMembersVerified());
        auto *content = window->contentItem();
        click(window, visibleItem(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, visibleItem(content, "topic-" + topicId));
        QTRY_VERIFY(visibleItem(content, "inline-image-" + first));
        auto *viewer = controller.mediaViewer();
        click(window, visibleItem(content, "inline-image-" + first));
        QTRY_VERIFY(viewer->open());
        QVERIFY(releaseDownload);
        // Review reproduction (exo-review finding=4 follow-up): while the slot
        // is still loading there is no reference media element to click, so a
        // click in the media area is a backdrop click and must close the
        // viewer, exactly like the reference placeholder markup allows.
        QVERIFY(!visibleItem(content, "mediaViewer-image"));
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(720, 450));
        QTRY_VERIFY(!viewer->open());
        if (releaseDownload) releaseDownload();
        releaseDownload = nullptr;

        // Once the bytes exist, a click inside the painted image must not close.
        click(window, visibleItem(content, "inline-image-" + first));
        QTRY_VERIFY(viewer->open());
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *shot = visibleItem(content, "mediaViewer-image"); return shot && shot->isVisible(); })(), 4000);
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(720, 450));
        QTest::qWait(150);
        QVERIFY(viewer->open());
        click(window, visibleItem(content, "mediaViewer-close"));
        QTRY_VERIFY(!viewer->open());
    }

    void mediaViewerSingleImageHidesNavigationAndStrip()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString fileA = QStringLiteral("9f000000-0000-4000-8000-00000000000a");
        const QString first = QStringLiteral("9e000000-0000-4000-8000-000000000001");
        const QByteArray png = avatarProbePng(false);
        WindowChannelFixture fixture;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto file = "/api/workspace/v1/messenger/files/" + fileA;
            if (path.startsWith(file)) {
                if (path == file) {
                    auto metadata = avatarProbeMetadata(fileA, png);
                    metadata["stream_uuid"] = streamId; metadata["name"] = "shot.png";
                    return Response{metadata};
                }
                if (path == file + "/actions/download") {
                    Response result; result.rawBody = png; result.rawContentType = "image/png";
                    return result;
                }
                return Response{QJsonObject{}, 404};
            }
            if (path.endsWith("/messages/") && request.method == "GET") {
                ++fixture.historyGets;
                return Response{QJsonArray{mediaViewerRow(first, fileA, png.size())}};
            }
            return fallback(request);
        };
        ApiClient api(nullptr, true); Session session(&api); FileClient files(&api);
        MessageImages images(&api, &session, &files);
        NativeController controller(&api, &session); controller.useFiles(&files);
        NativePreferences preferences(directory.filePath("single-image.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences, nullptr, nullptr, &images);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto closeBeforeEngine = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Single-image viewer window did not settle before engine teardown", __FILE__, __LINE__);
        });
        window->resize(1440, 900); QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
        session.login(fixture.server.origin(), "synthetic", "synthetic");
        QTRY_COMPARE(session.projects().size(), 1); session.selectProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1); QTRY_VERIFY(controller.channelMembersVerified());
        auto *content = window->contentItem();
        click(window, visibleItem(content, "stream-" + streamId));
        QTRY_COMPARE(controller.topics().size(), 1);
        click(window, visibleItem(content, "topic-" + topicId));
        QTRY_COMPARE(controller.selectedTopic(), topicId);
        QTRY_VERIFY(visibleItem(content, "inline-image-" + first));
        auto *viewer = controller.mediaViewer();
        click(window, visibleItem(content, "inline-image-" + first));
        QTRY_VERIFY(viewer->open());
        QCOMPARE(viewer->count(), 1);
        // The strip and the arrows are rendered only for galleries of more
        // than one item (media-viewer-thumbnails.ui.tsx:75, controls.ui.tsx:30).
        QVERIFY(!item(content, "mediaViewer-prev")->isVisible());
        QVERIFY(!item(content, "mediaViewer-next")->isVisible());
        QVERIFY(!item(content, "mediaViewer-strip")->isVisible());
        click(window, visibleItem(content, "mediaViewer-close"));
        QTRY_VERIFY(!viewer->open());
    }

    void topOverlayBannerRendersAboveSplitWithoutBlockingHeaderClicks()
    {
        WindowBannerFixture fixture;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("overlay-banners.ini"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Overlay banner teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(fixture.start(controller));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!visibleItem("connectionBanner"));
        QVERIFY(!visibleItem("bootstrapErrorBanner"));
        // A transport drop raises the critical connection banner over the split.
        fixture.dropRound = true;
        controller.refresh();
        QTRY_VERIFY(visibleItem("connectionBanner"));
        auto *banner = visibleItem("connectionBanner");
        QCOMPARE(banner->property("bannerText").toString(), QString("You are offline."));
        QCOMPARE(banner->property("accent").value<QColor>(), QColor("#d92d20"));
        auto *topBar = visibleItem("topBar"); auto *sidebar = visibleItem("sidebar");
        QVERIFY(topBar && sidebar);
        // The outage also fails the in-flight snapshot, so the bootstrap strip
        // may stack under the connection strip; measure the overlay layer.
        auto *overlay = visibleItem("topBannerOverlay"); QVERIFY(overlay);
        const auto overlayTop = overlay->mapToScene(QPointF(0, 0)).y();
        const auto headerBottom = topBar->mapToScene(QPointF(0, topBar->height())).y();
        const auto splitTop = sidebar->mapToScene(QPointF(0, 0)).y();
        QVERIFY2(overlayTop >= headerBottom - 1, "the overlay starts below the header");
        QVERIFY2(overlayTop <= splitTop + 1, "the overlay sits over the top of the split view");
        QVERIFY(banner->height() > 0);
        // Header clicks keep reaching the top bar while the banner is up.
        click(window, visibleItem("accountMenuButton"));
        QTRY_VERIFY(visibleItem("ownStatusAction"));
        click(window, overlay); // press outside closes the account popup
        QTRY_VERIFY(!visibleItem("ownStatusAction"));
        // The next successful response clears the banner again.
        fixture.dropRound = false;
        controller.refresh();
        QTRY_VERIFY(!visibleItem("connectionBanner"));
        // HTTP 429 renders the warning countdown and expires on its own.
        fixture.rateLimitAfterSeconds = 1;
        controller.refresh();
        QTRY_VERIFY(visibleItem("connectionBanner"));
        banner = visibleItem("connectionBanner");
        QVERIFY(banner->property("bannerText").toString().contains("Retrying in"));
        QVERIFY(banner->property("accent").value<QColor>() != QColor("#d92d20"));
        fixture.rateLimitAfterSeconds = 0;
        QTRY_VERIFY(!visibleItem("connectionBanner"));
        // A bootstrap failure shows the Retry banner; the header stays usable
        // and clicking Retry against the healthy server recovers the shell.
        fixture.breakStreams = true;
        controller.refresh();
        QTRY_VERIFY(visibleItem("bootstrapErrorBanner"));
        QVERIFY(visibleItem("bootstrapRetryButton"));
        QVERIFY(!visibleItem("bootstrapErrorText")->property("text").toString().isEmpty());
        click(window, visibleItem("accountMenuButton"));
        QTRY_VERIFY(visibleItem("ownStatusAction"));
        click(window, overlay); // press outside closes the account popup
        QTRY_VERIFY(!visibleItem("ownStatusAction"));
        fixture.breakStreams = false;
        click(window, visibleItem("bootstrapRetryButton"));
        QTRY_VERIFY(!visibleItem("bootstrapErrorBanner"));
        QTRY_COMPARE(controller.streams().size(), 1);
        QVERIFY(fixture.streamsGets >= 4);
    }

    // Issue #122: the reference expandable bottom toolbar persists its
    // expansion (settings.model.ts composerToolbarExpanded, default false)
    // and keeps 32px hit areas, h-7 w-px bg-border-subtle separators,
    // mr-[52px] send clearance and the pl-2 leading inset
    // (message-composer-toolbar.ui.tsx, message-composer-styles.lib.ts).
    void composerFormattingToolbarExpansionPersistsAcrossRecreation()
    {
        WindowChannelFixture fixture; fixture.sourceScene = true;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto settingsPath = directory.filePath("toolbar-expansion.ini");
        for (int lifetime = 0; lifetime < 3; ++lifetime) {
            NativePreferences preferences(settingsPath);
            preferences.setLocale("en"); preferences.setMode("light");
            ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
            QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
            auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
                if (guard) closeIncubatedTestWindow(engine, guard.data());
            });
            QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
            QVERIFY(startComposerEmojiFixture(fixture, controller, session));
            QTRY_VERIFY(controller.selectedTopic() == topicId);
            const auto view = [&](const QString &name) { return visibleItem(window->contentItem(), name); };
            auto *composer = view("messageComposer"); QVERIFY(composer && composer->isEnabled());
            auto *toolbar = item(window->contentItem(), "composerToolbarRow"); QVERIFY(toolbar);
            if (lifetime == 0) {
                // settings.model.ts:74: the default composer starts collapsed
                // with only the single expand toggle in the input row.
                QVERIFY(!toolbar->isVisible());
                auto *expandToggle = view("composerToolbarExpand"); QVERIFY(expandToggle);
                QVERIFY(!view("composerToolbarCollapse"));
                // message-composer-styles.lib.ts TOOLBAR_BTN h-8 w-8.
                QCOMPARE(expandToggle->width(), 32.0);
                QCOMPARE(expandToggle->height(), 32.0);
                const auto compactAxis = expandToggle->mapToItem(window->contentItem(), QPointF{}).x();
                click(window, expandToggle);
                QTRY_VERIFY(toolbar->isVisible());
                auto *collapseToggle = view("composerToolbarCollapse"); QVERIFY(collapseToggle);
                // COMPOSER_LEADING_CONTROLS_INSET pl-2: the collapse toggle
                // sits 8px from the toolbar row's left edge.
                QCOMPARE(qRound(collapseToggle->mapToItem(window->contentItem(), QPointF{}).x()
                    - toolbar->mapToItem(window->contentItem(), QPointF{}).x()), 8);
                // Reference comment: both 32px toggles stay on one vertical axis.
                QCOMPARE(qRound(collapseToggle->mapToItem(window->contentItem(), QPointF{}).x()),
                    qRound(compactAxis));
                // message-composer-styles.lib.ts TOOLBAR_BTN: every action
                // keeps the 32x32 hit area.
                const QStringList actions{"composerFormatLink", "composerFormatBold",
                    "composerFormatItalic", "composerFormatStrikethrough",
                    "composerFormatNumbered", "composerFormatBulleted",
                    "composerFormatQuote", "composerFormatCodeBlock",
                    "composerFormatCode", "composerFormatSpoiler"};
                for (const auto &name : actions) {
                    auto *action = view(name); QVERIFY2(action, qPrintable(name));
                    QVERIFY2(action->isEnabled(), qPrintable(name));
                    QCOMPARE(action->width(), 32.0);
                    QCOMPARE(action->height(), 32.0);
                }
                // message-composer-toolbar.ui.tsx separators: h-7 w-px bg-border-subtle.
                const auto separators = toolbar->findChildren<QQuickItem *>("composerToolbarSeparator");
                QCOMPARE(separators.size(), 3);
                for (auto *separator : separators) {
                    QCOMPARE(separator->width(), 1.0);
                    QCOMPARE(separator->height(), 28.0);
                    QCOMPARE(QQmlProperty::read(separator, "color").value<QColor>(),
                        QColor(window->property("colors").toMap().value("border-subtle").toString()));
                }
                // COMPOSER_TOOLBAR_SEND_CLEARANCE mr-[52px]: 40px send + 12px row gap.
                auto *send = view("sendButton"); QVERIFY(send);
                const auto sendRight = send->mapToItem(window->contentItem(), QPointF(send->width(), 0)).x();
                const auto toolbarRight
                    = toolbar->mapToItem(window->contentItem(), QPointF(toolbar->width(), 0)).x();
                QCOMPARE(qRound(sendRight - toolbarRight), 52);
                // The expansion now persists as true for the next lifetime.
            } else if (lifetime == 1) {
                QTRY_VERIFY_WITH_TIMEOUT(toolbar->isVisible(), 2000); // persisted expansion
                click(window, view("composerToolbarCollapse"));
                QTRY_VERIFY(!toolbar->isVisible()); // persisted collapse
            } else {
                QVERIFY(!toolbar->isVisible()); // collapsed choice survived recreation
                QVERIFY(view("composerToolbarExpand"));
            }
            QVERIFY(closeIncubatedTestWindow(engine, window)); close.dismiss();
        }
    }

    // Insert text per message-composer-toolbar.ui.tsx actions with the
    // wrap/selection-restore semantics of message-composer-selection.lib.ts.
    void composerToolbarInsertActionsProduceExactReferenceMarkers_data()
    {
        QTest::addColumn<QString>("button");
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("selectionStart");
        QTest::addColumn<int>("selectionEnd");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<int>("expectedStart");
        QTest::addColumn<int>("expectedEnd");
        // Wrap actions: empty selection gets the caret between the markers,
        // a selection collapses after the closing marker.
        QTest::newRow("bold-selection") << "composerFormatBold" << "word" << 0 << 4
            << QString("**word**") << 8 << 8;
        QTest::newRow("bold-empty") << "composerFormatBold" << "abcd" << 2 << 2
            << QString("ab****cd") << 4 << 4;
        QTest::newRow("italic-selection") << "composerFormatItalic" << "word" << 0 << 4
            << QString("*word*") << 6 << 6;
        QTest::newRow("italic-empty") << "composerFormatItalic" << "abcd" << 2 << 2
            << QString("ab**cd") << 3 << 3;
        QTest::newRow("inline-code-selection") << "composerFormatCode" << "word" << 0 << 4
            << QString("`word`") << 6 << 6;
        QTest::newRow("inline-code-empty") << "composerFormatCode" << "abcd" << 2 << 2
            << QString("ab``cd") << 3 << 3;
        QTest::newRow("strike-selection") << "composerFormatStrikethrough" << "word" << 0 << 4
            << QString("~~word~~") << 8 << 8;
        QTest::newRow("strike-empty") << "composerFormatStrikethrough" << "abcd" << 2 << 2
            << QString("ab~~~~cd") << 4 << 4;
        QTest::newRow("spoiler-selection") << "composerFormatSpoiler" << "word" << 0 << 4
            << QString("||word||") << 8 << 8;
        QTest::newRow("spoiler-empty") << "composerFormatSpoiler" << "abcd" << 2 << 2
            << QString("ab||||cd") << 4 << 4;
        // Per-line prefix actions.
        QTest::newRow("quote-selection") << "composerFormatQuote" << "word" << 0 << 4
            << QString("> word") << 6 << 6;
        QTest::newRow("quote-empty") << "composerFormatQuote" << "abcd" << 2 << 2
            << QString("ab> cd") << 4 << 4;
        QTest::newRow("quote-multiline") << "composerFormatQuote" << "a\nb" << 0 << 3
            << QString("> a\n> b") << 7 << 7;
        QTest::newRow("bulleted-selection") << "composerFormatBulleted" << "word" << 0 << 4
            << QString("- word") << 6 << 6;
        QTest::newRow("bulleted-empty") << "composerFormatBulleted" << "abcd" << 2 << 2
            << QString("ab- cd") << 4 << 4;
        QTest::newRow("bulleted-multiline") << "composerFormatBulleted" << "a\nb" << 0 << 3
            << QString("- a\n- b") << 7 << 7;
        QTest::newRow("numbered-selection") << "composerFormatNumbered" << "word" << 0 << 4
            << QString("1. word") << 7 << 7;
        QTest::newRow("numbered-empty") << "composerFormatNumbered" << "abcd" << 2 << 2
            << QString("ab1. cd") << 5 << 5;
        QTest::newRow("numbered-multiline") << "composerFormatNumbered" << "a\nb" << 0 << 3
            << QString("1. a\n2. b") << 9 << 9;
        // Fenced block and link template (URL selected).
        QTest::newRow("code-block-selection") << "composerFormatCodeBlock" << "word" << 0 << 4
            << QString("```\nword\n```") << 12 << 12;
        QTest::newRow("code-block-empty") << "composerFormatCodeBlock" << "abcd" << 2 << 2
            << QString("ab```\n\n```cd") << 6 << 6;
        QTest::newRow("link-selection") << "composerFormatLink" << "word" << 0 << 4
            << QString("[word](https://)") << 7 << 15;
        QTest::newRow("link-empty") << "composerFormatLink" << "abcd" << 2 << 2
            << QString("ab[link text](https://)cd") << 14 << 22;
        // exo-review #173: the URL slot search must be anchored to the end of
        // the inserted template, so a label that itself contains "https://"
        // cannot move the selection start into the label text.
        QTest::newRow("link-label-contains-url") << "composerFormatLink"
            << QString("https://example.com docs") << 0 << 24
            << QString("[https://example.com docs](https://)") << 27 << 35;
    }

    void composerToolbarInsertActionsProduceExactReferenceMarkers()
    {
        QFETCH(QString, button); QFETCH(QString, text);
        QFETCH(int, selectionStart); QFETCH(int, selectionEnd);
        QFETCH(QString, expected); QFETCH(int, expectedStart); QFETCH(int, expectedEnd);
        WindowChannelFixture fixture; fixture.sourceScene = true;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("toolbar-insert.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        preferences.setComposerToolbarExpanded(true);
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard) closeIncubatedTestWindow(engine, guard.data());
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(controller.selectedTopic() == topicId);
        const auto view = [&](const QString &name) { return visibleItem(window->contentItem(), name); };
        auto *composer = view("messageComposer"); QVERIFY(composer && composer->isEnabled());
        click(window, composer); QTRY_VERIFY(composer->hasActiveFocus());
        composer->setProperty("text", text);
        QTRY_COMPARE(controller.draft(), text);
        QTRY_VERIFY_WITH_TIMEOUT(bool(view(button)), 2000);
        auto *action = view(button); QVERIFY(action); QVERIFY2(action->isEnabled(), qPrintable(button));
        QMetaObject::invokeMethod(composer, "select", Q_ARG(int, selectionStart), Q_ARG(int, selectionEnd));
        QCOMPARE(composer->property("selectionEnd").toInt(), selectionEnd);
        click(window, action);
        QTRY_COMPARE(composer->property("text").toString(), expected);
        QCOMPARE(controller.draft(), expected);
        // Toolbar clicks must not steal the caret from the composer.
        QVERIFY(composer->hasActiveFocus());
        QCOMPARE(composer->property("selectionStart").toInt(), expectedStart);
        QCOMPARE(composer->property("selectionEnd").toInt(), expectedEnd);
    }

    // Shift+Enter continuation per message-composer-list-continuation.lib.ts:
    // matching marker/indent, incremented ordered numbers, and an empty
    // marker line is deleted instead of extended.
    void composerShiftEnterContinuesAndExitsMarkdownLists()
    {
        WindowChannelFixture fixture; fixture.sourceScene = true;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("toolbar-continuation.ini"));
        preferences.setLocale("en"); preferences.setMode("light");
        preferences.setComposerToolbarExpanded(true);
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard) closeIncubatedTestWindow(engine, guard.data());
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        QTRY_VERIFY(controller.selectedTopic() == topicId);
        auto *composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isEnabled());
        click(window, composer); QTRY_VERIFY(composer->hasActiveFocus());
        for (const auto character : QByteArray("- item")) QTest::keyClick(window, character);
        QTRY_COMPARE(composer->property("text").toString(), QString("- item"));
        // "continues unordered list item with matching marker": "- item" -> "- item\n- ".
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(composer->property("text").toString(), QString("- item\n- "));
        QCOMPARE(composer->property("cursorPosition").toInt(), 9);
        for (const auto character : QByteArray("two")) QTest::keyClick(window, character);
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(composer->property("text").toString(), QString("- item\n- two\n- "));
        // "exits the list on an empty marker line": the "- " line disappears.
        // replaceLineWithEmpty in message-composer-list-continuation.lib.ts
        // deletes [lineStart, lineEnd), so the previous line's terminator stays.
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(composer->property("text").toString(), QString("- item\n- two\n"));
        QCOMPARE(composer->property("cursorPosition").toInt(), 13);
        // "continues ordered list with incremented number": "3. item" -> "4. ".
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const auto character : QByteArray("3. item")) QTest::keyClick(window, character);
        QTRY_COMPARE(composer->property("text").toString(), QString("3. item"));
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(composer->property("text").toString(), QString("3. item\n4. "));
        QCOMPARE(composer->property("cursorPosition").toInt(), 11);
        // "exits ordered list on empty marker line" (line terminator retained).
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(composer->property("text").toString(), QString("3. item\n"));
        QCOMPARE(composer->property("cursorPosition").toInt(), 8);
        QCOMPARE(controller.draft(), QString("3. item\n"));
        // A plain Shift+Enter inside a non-list line still inserts a newline.
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const auto character : QByteArray("plain")) QTest::keyClick(window, character);
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(composer->property("text").toString(), QString("plain\n"));
        // Inside a list line (caret not at line end) the newline stays plain.
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const auto character : QByteArray("- item")) QTest::keyClick(window, character);
        QTest::keyClick(window, Qt::Key_Home);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(composer->property("text").toString(), QString("- \nitem"));
    }

    // ---- Issue #129 right drawer shell ----
    // Geometry and chrome contract: workspace_ui right-drawer.ui.tsx and
    // packages/web/tailwind.config.ts:84 (w-panel-right 315 px) at
    // 61dacef78dab2f5d0be76f283bebf49fe29b84af. Stack contract:
    // right-drawer.model.ts and right-drawer.model.test.ts at the same sha.

    void rightDrawerShellMatchesReferenceGeometry_data()
    {
        QTest::addColumn<QString>("locale"); QTest::addColumn<QString>("mode");
        QTest::newRow("en-light") << QString("en") << QString("light");
        QTest::newRow("ru-dark") << QString("ru") << QString("dark");
    }
    void rightDrawerShellMatchesReferenceGeometry()
    {
        QFETCH(QString, locale); QFETCH(QString, mode);
        RightDrawerHarness harness(locale, mode);
        // Reference aside: `w-panel-right ... rounded-lg bg-bg-elevated px-2 pb-5 pt-2`
        // (right-drawer.ui.tsx:17).
        harness.evaluate("rightDrawer.toggleChatInfo()");
        auto *drawer = visibleItem("nativeRightDrawer");
        QVERIFY2(drawer, "right-drawer.ui.tsx aside frame is required");
        QCOMPARE(drawer->width(), qreal(315)); // tailwind.config.ts:84 w-panel-right 315 px
        QCOMPARE(drawer->mapToScene(QPointF{}).x(), harness.window->width() - 315);
        QCOMPARE(qRound(drawer->height()), harness.window->height() - 8);
        auto *shell = visibleItem("nativeRightDrawerShell"); QVERIFY(shell);
        QCOMPARE(shell->property("radius").toReal(), qreal(6)); // rounded-lg aligned with the left sidebar
        QCOMPARE(shell->property("color").value<QColor>(),
            harness.window->property("colors").toMap().value("bg-elevated").value<QColor>());
        auto *header = visibleItem("nativeRightDrawerHeader"); QVERIFY(header);
        QCOMPARE(qRound(header->height()), 40);
        auto *body = visibleItem("nativeRightDrawerBody"); QVERIFY(body);
        QVERIFY(body->property("clip").toBool()); // flex-col overflow-hidden content slot
        const auto bodyTopLeft = body->mapToItem(drawer, QPointF(0, 0));
        QCOMPARE(qRound(bodyTopLeft.x()), 8); // px-2
        QCOMPARE(qRound(body->width()), 299); // 315 - 2*8
        QCOMPARE(qRound(bodyTopLeft.y()), 48); // pt-2 above the 40 px header
        QCOMPARE(qRound(drawer->height() - bodyTopLeft.y() - body->height()), 20); // pb-5
        // contentFlush cancels the shell px-2 on the direct body child (right-drawer.types.ts).
        harness.evaluate("rightDrawer.contentFlush = true");
        auto *flushedBody = visibleItem("nativeRightDrawerBody");
        QCOMPARE(qRound(flushedBody->mapToItem(drawer, QPointF(0, 0)).x()), 0);
        QCOMPARE(qRound(flushedBody->width()), 315);
        // aria label via the info panel key (a11y.infoPanel: "Info panel").
        auto *drawerAccessible = QAccessible::queryAccessibleInterface(drawer); QVERIFY(drawerAccessible);
        QCOMPARE(drawerAccessible->role(), QAccessible::ComplementaryContent);
        QCOMPARE(drawerAccessible->text(QAccessible::Name),
            locale == "ru" ? QString::fromUtf8("Панель информации") : QString("Info panel"));
    }

    void rightDrawerHeaderKeepsCloseAlwaysAndBackOnlyForDeeperScreens_data()
    {
        rightDrawerShellMatchesReferenceGeometry_data();
    }
    void rightDrawerHeaderKeepsCloseAlwaysAndBackOnlyForDeeperScreens()
    {
        QFETCH(QString, locale); QFETCH(QString, mode);
        RightDrawerHarness harness(locale, mode);
        harness.evaluate("rightDrawer.toggleChatInfo()");
        QVERIFY(visibleItem("nativeRightDrawer"));
        // Reference "keeps close button when title is omitted".
        QVERIFY(!visibleItem("nativeRightDrawerTitle"));
        auto *closeButton = visibleItem("nativeRightDrawerClose"); QVERIFY(closeButton);
        QCOMPARE(qRound(closeButton->width()), 32); QCOMPARE(qRound(closeButton->height()), 32);
        QCOMPARE(QQmlProperty(closeButton, "icon.width").read().toInt(), 16); // Icon size 16
        QVERIFY(!visibleItem("nativeRightDrawerBack")); // no deeper screen yet
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        auto *backButton = visibleItem("nativeRightDrawerBack"); QVERIFY(backButton);
        QCOMPARE(qRound(backButton->width()), 32); QCOMPARE(qRound(backButton->height()), 32);
        QCOMPARE(QQmlProperty(backButton, "icon.width").read().toInt(), 16);
        // aria labels via common.close / common.back.
        auto *closeAccessible = QAccessible::queryAccessibleInterface(closeButton); QVERIFY(closeAccessible);
        auto *backAccessible = QAccessible::queryAccessibleInterface(backButton); QVERIFY(backAccessible);
        QCOMPARE(closeAccessible->text(QAccessible::Name),
            locale == "ru" ? QString::fromUtf8("Закрыть") : QString("Close"));
        QCOMPARE(backAccessible->text(QAccessible::Name),
            locale == "ru" ? QString::fromUtf8("Назад") : QString("Back"));
        // The title shares the header row and truncates (truncate text-base h2).
        harness.evaluate("rightDrawer.title = 'Partner information for the current conversation'");
        auto *title = visibleItem("nativeRightDrawerTitle"); QVERIFY(title);
        QCOMPARE(QQmlProperty(title, "elide").read().toInt(), harness.evaluate("Text.ElideRight").toInt());
        QCOMPARE(title->width() < harness.window->width(), true);
        // Back pops only the internal screen: settings -> account root, back hidden again.
        click(harness.window, backButton);
        QTRY_VERIFY(!visibleItem("nativeRightDrawerBack"));
        QCOMPARE(harness.readString("currentAccountScreen"), QString("root"));
    }

    void rightDrawerPromotesExistingLayersInsteadOfDuplicating()
    {
        RightDrawerHarness harness;
        // Reference "promotes an existing account layer instead of duplicating it".
        harness.evaluate("rightDrawer.openUserMenu()");
        QCOMPARE(harness.readInt("layerCount"), 1);
        QCOMPARE(harness.readString("topLayerKind"), QString("account"));
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        QCOMPARE(harness.readInt("topScreenCount"), 2);
        harness.evaluate("rightDrawer.openInfo()");
        QCOMPARE(harness.readInt("layerCount"), 2);
        QCOMPARE(harness.readString("topLayerKind"), QString("chat-info"));
        // Promotion keeps a single chat-info layer: kinds stay [chat-info, account].
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        QCOMPARE(harness.readInt("layerCount"), 2);
        QCOMPARE(harness.readString("topLayerKind"), QString("account"));
        QCOMPARE(harness.readString("currentAccountScreen"), QString("settings"));
        harness.evaluate("rightDrawer.openInfo()");
        QCOMPARE(harness.readInt("layerCount"), 2);
        QVERIFY(visibleItem("nativeRightDrawer"));
        // "X closes the active layer and reveals the previous one".
        harness.evaluate("rightDrawer.closeCurrent()");
        QCOMPARE(harness.readString("topLayerKind"), QString("account"));
        QCOMPARE(harness.readString("currentAccountScreen"), QString("settings"));
        QVERIFY(visibleItem("nativeRightDrawer"));
        // "Closing the last layer hides the drawer and leaves an empty stack".
        harness.evaluate("rightDrawer.closeCurrent()");
        QVERIFY(!visibleItem("nativeRightDrawer"));
        QCOMPARE(harness.readInt("layerCount"), 0);
        QCOMPARE(harness.evaluate("rightDrawer.drawerOpen").toBool(), false);
    }

    void rightDrawerBackPopsNestedScreensOnly()
    {
        RightDrawerHarness harness;
        // "Back pops only internal screen history".
        harness.evaluate("rightDrawer.openUserMenu()");
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        harness.evaluate("rightDrawer.back()");
        QCOMPARE(harness.readString("currentAccountScreen"), QString("root"));
        QCOMPARE(harness.readInt("layerCount"), 1);
        harness.evaluate("rightDrawer.back()"); // root screen: back must not close the layer
        QCOMPARE(harness.readInt("layerCount"), 1);
        QVERIFY(visibleItem("nativeRightDrawer"));
        // Encrypted chat three-screen flow walks back without loops.
        harness.evaluate("rightDrawer.toggleChatInfo()");
        const auto scope = [&](const QString &screen) { return drawerChatScope("o1", 1, "s1", screen); };
        harness.evaluate("rightDrawer.openChatScreen(" + scope("settings") + ")");
        harness.evaluate("rightDrawer.openChatScreen(" + scope("key") + ")");
        QCOMPARE(harness.readInt("topScreenCount"), 3);
        QCOMPARE(harness.readString("currentChatScreen"), QString("key"));
        harness.evaluate("rightDrawer.back()");
        QCOMPARE(harness.readString("currentChatScreen"), QString("settings"));
        harness.evaluate("rightDrawer.back()");
        QCOMPARE(harness.readString("currentChatScreen"), QString(""));
        harness.evaluate("rightDrawer.back()");
        QCOMPARE(harness.readInt("topScreenCount"), 1);
        QCOMPARE(harness.readInt("layerCount"), 2); // both layers survive; only screens pop
        QVERIFY(visibleItem("nativeRightDrawer"));
        // "keeps local-key UUIDs distinct and returns to the existing list without a Back loop".
        harness.evaluate("rightDrawer.openChatScreen(" + scope("settings") + ")");
        harness.evaluate("rightDrawer.openChatScreen(" + scope("key") + ")");
        harness.evaluate("rightDrawer.openChatScreen(" + drawerChatScope("o1", 1, "s1", "local-key", "historical-a") + ")");
        harness.evaluate("rightDrawer.openChatScreen(" + drawerChatScope("o1", 1, "s1", "local-key", "historical-b") + ")");
        QCOMPARE(harness.readString("currentChatKeyUuid"), QString("historical-b"));
        harness.evaluate("rightDrawer.back()");
        QCOMPARE(harness.readString("currentChatKeyUuid"), QString("historical-a"));
        harness.evaluate("rightDrawer.openChatScreen(" + scope("key") + ")");
        QCOMPARE(harness.readInt("topScreenCount"), 3); // reuses the list position, no loop
        harness.evaluate("rightDrawer.back()");
        QCOMPARE(harness.readString("currentChatScreen"), QString("settings"));
        // A runtime generation change drops the chat screens back to the root.
        harness.evaluate("rightDrawer.openChatScreen(" + drawerChatScope("o1", 1, "s1", "local-key", "historical-a") + ")");
        harness.evaluate("rightDrawer.resetChatScreens({ownerKey:'o1',runtimeGeneration:2,streamUuid:'s1'})");
        QCOMPARE(harness.readString("currentChatScreen"), QString(""));
        QCOMPARE(harness.readInt("topScreenCount"), 1);
    }

    void rightDrawerIgnoresDuplicateTopScreenPush()
    {
        RightDrawerHarness harness;
        // "does not duplicate the active account screen".
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        QCOMPARE(harness.readInt("topScreenCount"), 2);
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        QCOMPARE(harness.readInt("topScreenCount"), 2);
        // An identical chat scope/screen is a no-op.
        const QString scope = drawerChatScope("o1", 1, "s1", "settings");
        harness.evaluate("rightDrawer.openChatScreen(" + scope + ")");
        const int chatScreens = harness.readInt("topScreenCount");
        harness.evaluate("rightDrawer.openChatScreen(" + scope + ")");
        QCOMPARE(harness.readInt("topScreenCount"), chatScreens);
        // A repeated profile override is a no-op; openInfo resets the nested profile.
        harness.evaluate("rightDrawer.openUserProfile(42)");
        QCOMPARE(harness.readString("topLayerKind"), QString("chat-info"));
        QCOMPARE(harness.readString("currentProfileOverride"), QString("legacy-user-id:42"));
        harness.evaluate("rightDrawer.openUserProfile(42)");
        QCOMPARE(harness.readInt("topScreenCount"), 3);
        harness.evaluate("rightDrawer.openInfo()");
        QCOMPARE(harness.readInt("topScreenCount"), 1);
        QCOMPARE(harness.readString("currentProfileOverride"), QString(""));
    }

    void rightDrawerCollapsesWithoutDeletingHistory()
    {
        RightDrawerHarness harness;
        // "collapses and expands without deleting layer or screen history".
        harness.evaluate("rightDrawer.openUserMenu()");
        harness.evaluate("rightDrawer.openAccountScreen('appearance')");
        harness.evaluate("rightDrawer.toggleVisibility()");
        QVERIFY(!visibleItem("nativeRightDrawer"));
        QCOMPARE(harness.readInt("layerCount"), 1);
        QCOMPARE(harness.readInt("topScreenCount"), 2);
        QCOMPARE(harness.readString("currentAccountScreen"), QString("appearance"));
        harness.evaluate("rightDrawer.toggleVisibility()");
        QVERIFY(visibleItem("nativeRightDrawer"));
        QCOMPARE(harness.readString("currentAccountScreen"), QString("appearance"));
        // Chat header toggle: opens chat-info, clicking again collapses, history intact.
        harness.evaluate("rightDrawer.toggleChatInfo()");
        QCOMPARE(harness.readString("topLayerKind"), QString("chat-info"));
        QVERIFY(visibleItem("nativeRightDrawer"));
        harness.evaluate("rightDrawer.toggleChatInfo()");
        QVERIFY(!visibleItem("nativeRightDrawer"));
        QCOMPARE(harness.readInt("layerCount"), 2);
        harness.evaluate("rightDrawer.toggleChatInfo()");
        QVERIFY(visibleItem("nativeRightDrawer"));
        // An empty stack seeds the chat-info root on expand (setOpen contract).
        harness.evaluate("rightDrawer.clearAll()");
        QVERIFY(!visibleItem("nativeRightDrawer"));
        QCOMPARE(harness.readInt("layerCount"), 0);
        harness.evaluate("rightDrawer.toggleVisibility()");
        QVERIFY(visibleItem("nativeRightDrawer"));
        QCOMPARE(harness.readInt("layerCount"), 1);
        QCOMPARE(harness.readString("topLayerKind"), QString("chat-info"));
    }

    void rightDrawerConversationSwitchDropsNestedChatScreens()
    {
        const QString betaId = "9d000000-0000-4000-8000-000000000001";
        MockServer server;
        server.handler = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/streams/"))
                return {QJsonArray{windowChannel(streamId, "Alpha"), windowChannel(betaId, "Beta")}};
            if (path.endsWith("/streams/" + streamId)) return {windowChannel(streamId, "Alpha")};
            if (path.endsWith("/streams/" + betaId)) return {windowChannel(betaId, "Beta")};
            if (path.endsWith("/users/"))
                return {QJsonArray{QJsonObject{{"uuid", userId}, {"username", "cassi"}}}};
            if (path.endsWith("/stream_topics/")) {
                const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                if (stream == betaId) return {QJsonArray{windowChannelTopic(topicId, betaId, "Beta General")}};
                return {QJsonArray{windowChannelTopic(topicId, streamId, "Alpha General")}};
            }
            if (path.endsWith("/messages/")) return {QJsonArray{}};
            return sessionResponse(request);
        };
        RightDrawerHarness harness;
        harness.controller.login(server.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([&]{ return harness.controller.projects().size() == 1; }, 3000))
            QFAIL("Right drawer mock session did not sign in");
        harness.controller.openProject(projectId);
        QTRY_COMPARE(harness.controller.streams().size(), 2);
        harness.controller.selectStream(streamId);
        QTRY_COMPARE(harness.controller.selectedStream(), streamId);
        harness.evaluate("rightDrawer.toggleChatInfo()");
        QVERIFY(visibleItem("nativeRightDrawer"));
        const int generation = harness.window->property("rightDrawerRuntimeGeneration").toInt();
        QVERIFY2(generation >= 1, "login must advance the right drawer runtime generation");
        const auto ownerKey = harness.evaluate("root.rightDrawerChatScope.ownerKey").toString();
        harness.evaluate("rightDrawer.openChatScreen(" + drawerChatScope(ownerKey, generation, streamId, "settings") + ")");
        harness.evaluate("rightDrawer.openChatScreen(" + drawerChatScope(ownerKey, generation, streamId, "key") + ")");
        QCOMPARE(harness.readInt("topScreenCount"), 3);
        // The same stream retains navigation.
        harness.evaluate("root.resetRightDrawerChatScreens()");
        QCOMPARE(harness.readInt("topScreenCount"), 3);
        // A conversation switch drops the other conversation's screens and keeps the root.
        harness.controller.selectStream(betaId);
        QTRY_COMPARE(harness.controller.selectedStream(), betaId);
        QTRY_COMPARE(harness.readInt("topScreenCount"), 1);
        QCOMPARE(harness.readString("topLayerKind"), QString("chat-info"));
        QCOMPARE(harness.readString("currentChatScreen"), QString(""));
        QVERIFY(visibleItem("nativeRightDrawer"));
        harness.controller.selectStream(streamId);
        QTRY_COMPARE(harness.controller.selectedStream(), streamId);
        QCOMPARE(harness.readInt("topScreenCount"), 1); // dropped screens are not restored
        // A runtime generation change resets hidden chat screens the same way.
        harness.evaluate("rightDrawer.openChatScreen(" + drawerChatScope(ownerKey, generation, streamId, "key") + ")");
        QCOMPARE(harness.readInt("topScreenCount"), 2);
        QVERIFY(harness.window->setProperty("rightDrawerRuntimeGeneration", generation + 1));
        QTRY_COMPARE(harness.readInt("topScreenCount"), 1);
        // Signing out clears the drawer entirely.
        harness.controller.logout();
        QTRY_VERIFY(!harness.controller.authenticated());
        QTRY_COMPARE(harness.readInt("layerCount"), 0);
        QVERIFY(!visibleItem("nativeRightDrawer"));
    }

    void rightDrawerEscapeClosesTopLayerThenHidesDrawer()
    {
        RightDrawerHarness harness;
        harness.evaluate("rightDrawer.toggleChatInfo()");
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        QVERIFY(visibleItem("nativeRightDrawer"));
        QTest::keyClick(harness.window, Qt::Key_Escape);
        QCOMPARE(harness.readString("topLayerKind"), QString("chat-info"));
        QVERIFY(visibleItem("nativeRightDrawer"));
        QTest::keyClick(harness.window, Qt::Key_Escape);
        QTRY_VERIFY(!visibleItem("nativeRightDrawer"));
        QCOMPARE(harness.readInt("layerCount"), 0);
    }

    void rightDrawerRootRequestSelectsAccountRootAfterPromotion()
    {
        RightDrawerHarness harness;
        // Requesting the account root after an existing account layer is
        // promoted must select the root screen, not keep a stale nested one.
        harness.evaluate("rightDrawer.openUserMenu()");
        QCOMPARE(harness.readString("topLayerKind"), QString("account"));
        QCOMPARE(harness.readString("currentAccountScreen"), QString("root"));
        // Navigate the account layer into a nested settings screen.
        harness.evaluate("rightDrawer.openAccountScreen('settings')");
        QCOMPARE(harness.readString("currentAccountScreen"), QString("settings"));
        // Move to another layer, then request the account root again: the
        // promoted layer must end up on the account root, not on settings.
        harness.evaluate("rightDrawer.openInfo()");
        QCOMPARE(harness.readString("topLayerKind"), QString("chat-info"));
        harness.evaluate("rightDrawer.openAccountScreen('root')");
        QCOMPARE(harness.readString("topLayerKind"), QString("account"));
        QCOMPARE(harness.readInt("layerCount"), 2);
        QCOMPARE(harness.readString("currentAccountScreen"), QString("root"));
        QVERIFY(harness.evaluate("rightDrawer.canGoBack").toBool());
        // Back pops the internal history: the nested settings screen is intact.
        harness.evaluate("rightDrawer.back()");
        QCOMPARE(harness.readString("currentAccountScreen"), QString("settings"));
        // A fresh account layer keeps a single root screen (no duplicate push).
        harness.evaluate("rightDrawer.clearAll()");
        harness.evaluate("rightDrawer.openAccountScreen('root')");
        QCOMPARE(harness.readInt("layerCount"), 1);
        QCOMPARE(harness.readInt("topScreenCount"), 1);
        QCOMPARE(harness.readString("currentAccountScreen"), QString("root"));
    }

    // Issue #132: header summary (48px avatar with the stream color, title,
    // "X members, Y online" subline) plus the three blocks in reference
    // order. Oracles: right-panel-workspace-info.ui.tsx at
    // 61dacef78dab2f5d0be76f283bebf49fe29b84af (header px-4 py-3, avatar lg
    // h-12 w-12 = 48px, title text-sm = 14px font-medium, subline text-[11px]
    // text-secondary; blocks inside p-2 space-y-5 = 8px padding, 20px gaps)
    // and packages/web/src/shared/lib/avatar-color.lib.ts for the color.
    void windowChannelInfoHeaderSummary()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QVERIFY(visibleItem("channelInfoHeader"));
        auto *avatar = visibleItem("channelInfoAvatar");
        QVERIFY(avatar);
        QCOMPARE(avatar->width(), 48.0);   // avatar.tsx SIZE_CLASS.lg h-12 w-12
        QCOMPARE(avatar->height(), 48.0);
        // color oracle: resolveAvatarBackgroundColor(0x4b98e6) -> #4b98e6
        QCOMPARE(avatar->property("color").value<QColor>(), QColor("#4b98e6"));
        auto *title = visibleItem("channelInfoTitle");
        QVERIFY(title);
        QCOMPARE(title->property("text").toString(), QStringLiteral("Info channel"));
        // subline is `${count} members, ${online} online`; counts wait on the
        // members page (messenger-chat-header.lib.ts XChatOnlineCountPair).
        QTRY_COMPARE_WITH_TIMEOUT(scene.text("channelInfoSubline"),
            QStringLiteral("4 members, 2 online"), 5000);
        auto *content = scene.window->contentItem();
        QVERIFY(hasVisibleText(content, QStringLiteral("Notifications")));
        QVERIFY(hasVisibleText(content, QStringLiteral(" (For entire chat)")));
        QVERIFY(hasVisibleText(content, QStringLiteral("Description")));
        QVERIFY(hasVisibleText(content, QStringLiteral("Synthetic purpose.")));
        QVERIFY(hasVisibleText(content, QStringLiteral("Members")));
        QVERIFY(visibleItem("channelInfoNotificationSwitch"));
        QVERIFY(!visibleItem("channelInfoNotificationUnsupported"));
        // block order follows the reference: notifications, description, members
        const auto switchY = visibleItem("channelInfoNotificationSwitch")
            ->mapToScene(QPointF(0, 0)).y();
        const auto descY = visibleItem("channelInfoDescriptionHeading")
            ->mapToScene(QPointF(0, 0)).y();
        const auto membersY = visibleItem("channelInfoMembersHeading")
            ->mapToScene(QPointF(0, 0)).y();
        QVERIFY(switchY < descY);
        QVERIFY(descY < membersY);
        // title column starts after the 48px avatar plus the gap-4 (16px) gap
        const auto avatarPos = avatar->mapToScene(QPointF(0, 0));
        const auto titlePos = title->mapToScene(QPointF(0, 0));
        QVERIFY(titlePos.x() >= avatarPos.x() + 48.0 + 16.0 - 1.0);
    }

    // Issue #132: the notification switch (notification-level-switch.lib.ts:
    // subscribed=all_messages bell, mentions_only at, muted bell_off; the whole
    // switch is disabled while the update POST runs; the request body is
    // {notification_mode} against streams/{id}/actions/notifications/invoke,
    // and a click on the current level sends nothing).
    void windowChannelInfoNotificationSwitchPersists()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        QCOMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("all_messages"));
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTRY_VERIFY(scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        // the whole switch is disabled during the in-flight update (isDisabled)
        QVERIFY(!visibleItem("channelInfoNotifSubscribed")->property("enabled").toBool());
        QVERIFY(!visibleItem("channelInfoNotifDefault")->property("enabled").toBool());
        QTRY_VERIFY(!scene.fixture.notifBody.isEmpty());
        QCOMPARE(scene.fixture.notifBody.value("notification_mode").toString(),
            QStringLiteral("muted"));
        QTRY_VERIFY(!scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QTRY_COMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("muted"));
        QVERIFY(scene.text("channelInfoNotificationError").isEmpty());
        QVERIFY(visibleItem("channelInfoNotifMuted")->property("enabled").toBool());
        click(scene.window, visibleItem("channelInfoNotifMuted"));  // same level: no request
        QTest::qWait(250);
        QCOMPARE(scene.fixture.notifPosts, 1);
        QCOMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("muted"));
    }

    // Issue #132: a failed level update keeps the previous mode and surfaces
    // the reference failure text (t("app.error") -> "Something went wrong",
    // notification-level-switch.lib.ts), under the switch, and a later
    // success recovers and clears it.
    void windowChannelInfoNotificationSwitchFailureKeepsError()
    {
        ChannelInfoWindowScene scene;
        scene.fixture.notifStatus = 500;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTRY_COMPARE(scene.text("channelInfoNotificationError"), QStringLiteral("Something went wrong"));
        QVERIFY(!scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QVERIFY(visibleItem("channelInfoNotifMuted")->property("enabled").toBool());
        QCOMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("all_messages"));  // failed update keeps the old level
        scene.fixture.notifStatus = 200;
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTRY_COMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("muted"));
        QTRY_VERIFY(visibleItem("channelInfoNotificationError") == nullptr);
    }

    // Issue #132: member rows (WorkspaceMemberCard: sm avatar with presence
    // dot, name text-sm, role chip text-[10px], secondary text-[11px] with
    // resolveWorkspaceStatusLabel(status) ?? email fallback) plus the self
    // removal affordance against DELETE /stream_bindings/{uuid}; the owner
    // sees removal on every row (canRemove = own || stream owner).
    void windowChannelInfoMemberRowsAndSelfRemoval()
    {
        ChannelInfoWindowScene scene;
        const auto selfBinding = scene.fixture.bindingId(1);
        scene.openChannelInfo();
        QTRY_VERIFY(visibleItem("channelInfoMember-" + selfBinding));
        QVERIFY(visibleItem("channelInfoMember-" + scene.fixture.bindingId(2)));
        QVERIFY(visibleItem("channelInfoMember-" + scene.fixture.bindingId(4)));
        // role chip labels (roleLabel): Owner on the own row, Administrator
        // on the moderator twin row
        QCOMPARE(scene.text("channelInfoMemberRole-" + selfBinding), QStringLiteral("Owner"));
        QCOMPARE(scene.text("channelInfoMemberRole-" + scene.fixture.bindingId(3)),
            QStringLiteral("Administrator"));
        // display names come from the verified user directory
        QCOMPARE(scene.text("channelInfoMemberName-" + scene.fixture.bindingId(2)),
            QStringLiteral("Alice"));
        // presence line: active renders the online status label
        QCOMPARE(scene.text("channelInfoMemberSecondary-" + selfBinding), QStringLiteral("online"));
        // unknown presence falls back to the email (status ?? email oracle)
        QCOMPARE(scene.text("channelInfoMemberSecondary-" + scene.fixture.bindingId(4)),
            QStringLiteral("carol@example.invalid"));
        // the stream owner can ask removal on foreign rows and on the own row
        QVERIFY(visibleItem("channelInfoRemove-" + scene.fixture.bindingId(2)));
        QVERIFY(visibleItem("channelInfoRemove-" + selfBinding));
        click(scene.window, visibleItem("channelInfoRemove-" + selfBinding));
        QTRY_COMPARE(scene.fixture.removeDeletes, 1);
        QTRY_VERIFY(!visibleItem("channelInfoMember-" + selfBinding));
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 3);
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("3 members, 1 online"));
        QVERIFY(scene.text("channelInfoMemberError").isEmpty());
    }

    // Issue #132: the owner-only Add members dialog (reference lines 99-246):
    // the picker is the user directory minus current members, filtering by
    // name/username/email, single selection posts exactly {member:[uuid]} to
    // streams/{id}/actions/add_users/invoke and the response bindings append
    // to the member list before the dialog closes.
    void windowChannelInfoAddMemberFlow()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        QVERIFY(visibleItem("channelInfoAddButton"));   // owner gate (ShowAddUsersModal)
        click(scene.window, visibleItem("channelInfoAddButton"));
        QTRY_VERIFY(scene.evaluate("channelInfoLayer.addDialogOpen").toBool());
        // current members never appear in the picker
        QVERIFY(!visibleItem("addMembersOption-" + channelPeer));
        QVERIFY(!visibleItem("addMembersOption-" + scene.fixture.bobUser));
        // the stranger dave is available and matches by name/email
        QVERIFY(visibleItem("addMembersOption-" + scene.fixture.daveUser));
        auto *search = visibleItem("addMembersSearch");
        QVERIFY(search);
        search->setProperty("text", "zzz");
        QTRY_VERIFY(!visibleItem("addMembersOption-" + scene.fixture.daveUser));
        QTRY_VERIFY(visibleItem("addMembersEmpty"));
        search->setProperty("text", "dav");
        QTRY_VERIFY(visibleItem("addMembersOption-" + scene.fixture.daveUser));
        auto *addButton = visibleItem("addMembersAdd");
        QVERIFY(addButton);
        QVERIFY(!addButton->property("enabled").toBool());  // nothing selected yet
        click(scene.window, visibleItem("addMembersOption-" + scene.fixture.daveUser));
        QTRY_VERIFY(visibleItem("addMembersCheck-" + scene.fixture.daveUser));
        QTRY_VERIFY(addButton->property("enabled").toBool());
        click(scene.window, addButton);
        QTRY_VERIFY(!scene.evaluate("channelInfoLayer.addDialogOpen").toBool());
        QTRY_VERIFY(scene.fixture.memberBody.value("member").toArray().size() == 1);
        QCOMPARE(scene.fixture.memberBody.value("member").toArray().at(0).toString(),
            scene.fixture.daveUser);
        QTRY_VERIFY(scene.evaluate("controller.channelInfoAddSettled").toInt() > 0);
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 5);
        QVERIFY(scene.text("addMembersError").isEmpty());
        QVERIFY(visibleItem("channelInfoMember-83000000-0000-4000-8000-000000000005"));
    }

    // Issue #132: non-owner gating: no Add affordance and no removal controls
    // on foreign rows; the controller also refuses remove/add mutations for a
    // stream whose role is not owner (server gate mirrored client-side).
    void windowChannelInfoNonOwnerMemberGates()
    {
        ChannelInfoWindowScene scene(false);
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        QVERIFY(!visibleItem("channelInfoAddButton"));
        QVERIFY(!visibleItem("channelInfoRemove-" + scene.fixture.bindingId(2)));
        QVERIFY(!visibleItem("channelInfoRemove-" + scene.fixture.bindingId(4)));
        QVERIFY(visibleItem("channelInfoRemove-" + scene.fixture.bindingId(1)));  // leave channel
        scene.controller.removeChannelInfoMember(scene.fixture.bindingId(2));  // gated no-op
        QTest::qWait(250);
        QCOMPARE(scene.fixture.removeDeletes, 0);
        scene.controller.addChannelInfoMembers({scene.fixture.daveUser});       // gated no-op
        QTest::qWait(250);
        QCOMPARE(scene.fixture.memberPosts, 0);
    }

    // Issue #186 review F1: switching streams during a notification update must
    // not permanently disable the switch. The notifications POST for the first
    // stream is held by a defer gate; selectStream moves to the second stream
    // (the page refreshes there); the late response may only release the busy
    // state that belongs to its own request, so the switch on the new stream
    // stays clickable and a follow-up update completes.
    void windowChannelInfoNotificationBusyRecoversOnStreamSwitch()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        bool holdNotif = true;
        std::function<void()> releaseNotif;
        auto base = scene.fixture.server.handler;
        // The guard restores the fixture handler and releases any held send
        // before the scene tears down; the wrapper captures base by value so a
        // late socket event during ~scene never touches a dead local.
        auto gate = qScopeGuard([&]{ scene.fixture.server.handler = base;
            if (releaseNotif) releaseNotif(); });
        scene.fixture.server.handler = [base, &holdNotif, &releaseNotif](const Request &request) -> Response {
            auto response = base(request);
            if (holdNotif && request.method == "POST"
                && request.target.path().endsWith("/actions/notifications/invoke")) {
                holdNotif = false;
                response.defer = [&releaseNotif](std::function<void()> send) { releaseNotif = std::move(send); };
            }
            return response;
        };
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTRY_VERIFY(scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QTRY_COMPARE(scene.fixture.notifPosts, 1);
        scene.controller.selectStream(scene.fixture.secondStreamId);
        QTRY_COMPARE(scene.text("channelInfoTitle"), QStringLiteral("Second channel"));
        releaseNotif(); releaseNotif = {};
        // The abandoned request settles for a stream the user left: it must
        // release its busy state instead of locking every later stream.
        QTRY_VERIFY(!scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QTRY_VERIFY(visibleItem("channelInfoNotifMuted")->property("enabled").toBool());
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTRY_COMPARE(scene.fixture.notifPosts, 2);
        QTRY_COMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("muted"));
    }

    // Issue #186 review F2: same shape as F1 for the add-members flow: the
    // add POST for the first stream is held, the user switches streams, and
    // the late response must not leave channelInfoAddBusy set so that later
    // add requests are rejected on every stream.
    void windowChannelInfoAddBusyRecoversOnStreamSwitch()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        click(scene.window, visibleItem("channelInfoAddButton"));
        QTRY_VERIFY(scene.evaluate("channelInfoLayer.addDialogOpen").toBool());
        click(scene.window, visibleItem("addMembersOption-" + scene.fixture.daveUser));
        QTRY_VERIFY(visibleItem("addMembersCheck-" + scene.fixture.daveUser));
        bool holdAdd = true;
        std::function<void()> releaseAdd;
        auto base = scene.fixture.server.handler;
        auto gate = qScopeGuard([&]{ scene.fixture.server.handler = base;
            if (releaseAdd) releaseAdd(); });
        scene.fixture.server.handler = [base, &holdAdd, &releaseAdd](const Request &request) -> Response {
            auto response = base(request);
            if (holdAdd && request.method == "POST"
                && request.target.path().endsWith("/actions/add_users/invoke")) {
                holdAdd = false;
                response.defer = [&releaseAdd](std::function<void()> send) { releaseAdd = std::move(send); };
            }
            return response;
        };
        click(scene.window, visibleItem("addMembersAdd"));
        QTRY_VERIFY(scene.evaluate("controller.channelInfoAddBusy").toBool());
        QTRY_COMPARE(scene.fixture.memberPosts, 1);
        scene.controller.selectStream(scene.fixture.secondStreamId);
        QTRY_COMPARE(scene.text("channelInfoTitle"), QStringLiteral("Second channel"));
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 2);
        releaseAdd(); releaseAdd = {};
        QTRY_VERIFY(!scene.evaluate("controller.channelInfoAddBusy").toBool());
        // A later add request on the new stream is accepted, not rejected by
        // the leaked busy flag.
        scene.controller.addChannelInfoMembers({scene.fixture.daveUser});
        QTRY_COMPARE(scene.fixture.memberPosts, 2);
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 3);
    }

    // Issue #186 review round-2 G1: the notification busy state must be tracked
    // per stream by request token, not as one shared slot. A #1 is deferred;
    // switching to B and starting B's update may not take over A's tracking
    // (at head the single busy-stream slot was replaced, so after A->B->A a
    // second A update was accepted while A #1 was still in flight and that
    // first response could clear the newer request's busy state). Back on A
    // the follow-up update must be rejected, and the responses released out of
    // order (B first) may each only commit their own stream's state.
    void windowChannelInfoNotificationBusyIsPerStreamRequestScoped()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        bool deferNext = false;
        std::vector<std::function<void()>> releases;
        auto base = scene.fixture.server.handler;
        // The guard restores the fixture handler and releases any still-held
        // send before the scene tears down (head accepts an extra A request
        // whose held response the earlier assertions then never release).
        auto gate = qScopeGuard([&]{ scene.fixture.server.handler = base;
            for (auto &release : releases) if (release) release(); });
        scene.fixture.server.handler = [base, &deferNext, &releases](const Request &request) -> Response {
            auto response = base(request);
            if (deferNext && request.method == "POST"
                && request.target.path().endsWith("/actions/notifications/invoke")) {
                deferNext = false;
                response.defer = [&releases](std::function<void()> send)
                { releases.push_back(std::move(send)); };
            }
            return response;
        };
        // A #1: held by the gate.
        deferNext = true;
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTRY_VERIFY(scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QTRY_COMPARE(scene.fixture.notifPosts, 1);
        // B's update: also held; at head this replaces the tracked stream.
        scene.controller.selectStream(scene.fixture.secondStreamId);
        QTRY_COMPARE(scene.text("channelInfoTitle"), QStringLiteral("Second channel"));
        deferNext = true;
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTRY_COMPARE(scene.fixture.notifPosts, 2);
        // Back on A: A's own in-flight update keeps A busy (the B request may
        // not steal it), so the switch stays disabled and a second A update is
        // rejected instead of racing the first one.
        scene.controller.selectStream(streamId);
        QTRY_VERIFY(scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QVERIFY(!visibleItem("channelInfoNotifMuted")->property("enabled").toBool());
        deferNext = true;
        click(scene.window, visibleItem("channelInfoNotifMuted"));
        QTest::qWait(250);
        QCOMPARE(scene.fixture.notifPosts, 2);
        QVERIFY(scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        // Out-of-order release: B's response settles only B and never touches
        // A's busy state or A's row.
        releases[1](); releases[1] = {};
        QTest::qWait(250);
        QVERIFY(scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QCOMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("all_messages"));
        // A #1 is the current request for A: it commits and releases its own
        // stream only after that.
        releases[0](); releases[0] = {};
        QTRY_VERIFY(!scene.evaluate("controller.channelInfoNotificationBusy").toBool());
        QTRY_COMPARE(scene.evaluate("controller.channelInfoNotificationMode").toString(),
            QStringLiteral("muted"));
        QVERIFY(scene.text("channelInfoNotificationError").isEmpty());
    }

    // Issue #186 review round-2 G2: same request-token shape for the add flow.
    // A #1 is deferred; B's held add may not take over A's tracking; back on A
    // a second add is rejected, and the out-of-order releases each touch only
    // their own stream (dave lands exactly once, once A's response arrives).
    void windowChannelInfoAddBusyIsPerStreamRequestScoped()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        bool deferNext = false;
        std::vector<std::function<void()>> releases;
        auto base = scene.fixture.server.handler;
        auto gate = qScopeGuard([&]{ scene.fixture.server.handler = base;
            for (auto &release : releases) if (release) release(); });
        scene.fixture.server.handler = [base, &deferNext, &releases](const Request &request) -> Response {
            auto response = base(request);
            if (deferNext && request.method == "POST"
                && request.target.path().endsWith("/actions/add_users/invoke")) {
                deferNext = false;
                response.defer = [&releases](std::function<void()> send)
                { releases.push_back(std::move(send)); };
            }
            return response;
        };
        deferNext = true;
        scene.controller.addChannelInfoMembers({scene.fixture.daveUser});   // A #1 held
        QTRY_VERIFY(scene.evaluate("controller.channelInfoAddBusy").toBool());
        QTRY_COMPARE(scene.fixture.memberPosts, 1);
        scene.controller.selectStream(scene.fixture.secondStreamId);
        QTRY_COMPARE(scene.text("channelInfoTitle"), QStringLiteral("Second channel"));
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 2);
        deferNext = true;
        scene.controller.addChannelInfoMembers({scene.fixture.daveUser});   // B held
        QTRY_COMPARE(scene.fixture.memberPosts, 2);
        scene.controller.selectStream(streamId);
        // Note: the mock's add handler already mutated the served member list
        // when it received the (still-held) A #1 POST, so A reloads with dave
        // present; the client-side row commit is deduplicated by binding uuid.
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 5);
        // Back on A: A's own in-flight add keeps A busy; the follow-up add is
        // rejected, so no second A response can race the first one.
        QVERIFY(scene.evaluate("controller.channelInfoAddBusy").toBool());
        deferNext = true;
        scene.controller.addChannelInfoMembers({scene.fixture.daveUser});
        QTest::qWait(250);
        QCOMPARE(scene.fixture.memberPosts, 2);
        QVERIFY(scene.evaluate("controller.channelInfoAddBusy").toBool());
        // Out-of-order release: B's response settles only B's request.
        releases[1](); releases[1] = {};
        QTest::qWait(250);
        QVERIFY(scene.evaluate("controller.channelInfoAddBusy").toBool());
        QCOMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 5);
        // A #1 commits exactly once for A: dave appears and the busy slot frees.
        releases[0](); releases[0] = {};
        QTRY_VERIFY(!scene.evaluate("controller.channelInfoAddBusy").toBool());
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 5);
        QVERIFY(scene.evaluate("controller.channelInfoAddSettled").toInt() > 0);
        QVERIFY(scene.text("channelInfoAddError").isEmpty());
    }

    // Issue #186 review F3: closing the add-members dialog must reset the
    // dialog's own selection and search text (the old handler wrote to a
    // non-existent page property, so both survived a reopen).
    void windowChannelInfoAddDialogClearsSelectionOnClose()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        click(scene.window, visibleItem("channelInfoAddButton"));
        QTRY_VERIFY(scene.evaluate("channelInfoLayer.addDialogOpen").toBool());
        // The QQC2 Popup is a QObject (not a QQuickItem), so reach it through
        // the object tree; its object survives close/open cycles.
        auto *dialog = scene.window->findChild<QObject *>("addMembersDialog", Qt::FindChildrenRecursively);
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ return visibleItem("addMembersSearch"); })(), 5000);
        auto *search = visibleItem("addMembersSearch");
        QVERIFY(search);
        search->setProperty("text", "dav");
        QTRY_VERIFY(visibleItem("addMembersOption-" + scene.fixture.daveUser));
        click(scene.window, visibleItem("addMembersOption-" + scene.fixture.daveUser));
        QTRY_VERIFY(visibleItem("addMembersCheck-" + scene.fixture.daveUser));
        QCOMPARE(dialog->property("selectedUsers").toList().size(), 1);
        click(scene.window, visibleItem("addMembersClose"));
        QTRY_VERIFY(!scene.evaluate("channelInfoLayer.addDialogOpen").toBool());
        click(scene.window, visibleItem("channelInfoAddButton"));
        QTRY_VERIFY(scene.evaluate("channelInfoLayer.addDialogOpen").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ return visibleItem("addMembersSearch"); })(), 5000);
        QVERIFY2(dialog->property("selectedUsers").toList().isEmpty(),
            "closed dialog must clear its own selection");
        QVERIFY2(visibleItem("addMembersSearch")->property("text").toString().isEmpty(),
            "closed dialog must clear its search text");
        QVERIFY(!visibleItem("addMembersAdd")->property("enabled").toBool());
    }

    // Issue #186 review F4: a members GET that is still in flight when a
    // member mutation succeeds must not overwrite that mutation. The member
    // load is held by a defer gate (snapshot without dave), the add POST
    // completes, and the released stale snapshot may not remove the added row.
    void windowChannelInfoStaleMembersLoadKeepsAddedMember()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        bool holdMembers = true;
        std::function<void()> releaseMembers;
        auto base = scene.fixture.server.handler;
        auto gate = qScopeGuard([&]{ scene.fixture.server.handler = base;
            if (releaseMembers) releaseMembers(); });
        scene.fixture.server.handler = [base, &holdMembers, &releaseMembers](const Request &request) -> Response {
            auto response = base(request);
            if (holdMembers && request.method == "GET"
                && request.target.path().endsWith("/stream_bindings/")) {
                holdMembers = false;
                response.defer = [&releaseMembers](std::function<void()> send) { releaseMembers = std::move(send); };
            }
            return response;
        };
        // Re-run the member load exactly like a drawer reopen would.
        scene.controller.refreshChannelInfo();
        QTRY_VERIFY(scene.controller.channelInfoLoading());
        scene.controller.addChannelInfoMembers({scene.fixture.daveUser});
        QTRY_VERIFY(scene.fixture.memberPosts >= 1);
        QTRY_COMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 5);
        releaseMembers(); releaseMembers = {};
        QTest::qWait(400);  // let the held stale snapshot arrive and settle
        QCOMPARE(scene.evaluate("controller.channelInfoMemberCount").toInt(), 5);
        QVERIFY(visibleItem("channelInfoMember-83000000-0000-4000-8000-000000000005"));
    }

    // Issue #186 review F5: a selection made in the add-members dialog must
    // not survive a stream change; reopening or keeping the dialog open on the
    // new stream may not submit the earlier stream's user list.
    void windowChannelInfoAddDialogClearsSelectionOnStreamSwitch()
    {
        ChannelInfoWindowScene scene;
        scene.openChannelInfo();
        QTRY_COMPARE(scene.text("channelInfoSubline"), QStringLiteral("4 members, 2 online"));
        click(scene.window, visibleItem("channelInfoAddButton"));
        QTRY_VERIFY(scene.evaluate("channelInfoLayer.addDialogOpen").toBool());
        auto *dialog = scene.window->findChild<QObject *>("addMembersDialog", Qt::FindChildrenRecursively);
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ return visibleItem("addMembersSearch"); })(), 5000);
        visibleItem("addMembersSearch")->setProperty("text", "dav");
        QTRY_VERIFY(visibleItem("addMembersOption-" + scene.fixture.daveUser));
        click(scene.window, visibleItem("addMembersOption-" + scene.fixture.daveUser));
        QTRY_VERIFY(visibleItem("addMembersCheck-" + scene.fixture.daveUser));
        QCOMPARE(dialog->property("selectedUsers").toList().size(), 1);
        scene.controller.selectStream(scene.fixture.secondStreamId);
        QTRY_COMPARE(scene.text("channelInfoTitle"), QStringLiteral("Second channel"));
        QVERIFY2(dialog->property("selectedUsers").toList().isEmpty(),
            "stream switch must clear the dialog selection");
        QVERIFY2(visibleItem("addMembersSearch")->property("text").toString().isEmpty(),
            "stream switch must clear the dialog search text");
        QVERIFY(!visibleItem("addMembersAdd")->property("enabled").toBool());
    }

private:
    struct RightDrawerHarness {
        QTemporaryDir directory;
        NativePreferences preferences;
        ApiClient api{nullptr, true};
        Session session;
        NativeController controller;
        QQmlApplicationEngine engine;
        QPointer<QQuickWindow> window;
        explicit RightDrawerHarness(const QString &locale = QString(), const QString &mode = QString())
            : preferences(directory.filePath("preferences.ini")), session(&api), controller(&api, &session)
        {
            QVERIFY(directory.isValid());
            if (!locale.isEmpty()) preferences.setLocale(locale);
            if (!mode.isEmpty()) preferences.setMode(mode);
            loadNativeUi(engine, &controller, &preferences);
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            QVERIFY(window);
            QVERIFY(QTest::qWaitForWindowExposed(window));
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
        ~RightDrawerHarness()
        {
            if (window && !closeIncubatedTestWindow(engine, window))
                QTest::qFail("Right drawer shell teardown", __FILE__, __LINE__);
        }
        QVariant evaluate(const QString &script)
        {
            QQmlExpression expression(qmlContext(window), window, script);
            auto value = expression.evaluate();
            if (expression.hasError())
                QTest::qFail(qPrintable(QString("QML: ") + expression.error().toString()), __FILE__, __LINE__);
            if (value.metaType() == QMetaType::fromType<QJSValue>()) value = value.value<QJSValue>().toVariant();
            return value;
        }
        int readInt(const QString &name) { return evaluate("rightDrawer." + name).toInt(); }
        QString readString(const QString &name) { return evaluate("rightDrawer." + name).toString(); }
    };
    static QString drawerChatScope(const QString &ownerKey, int runtimeGeneration,
        const QString &streamUuid, const QString &screen, const QString &keyUuid = QString())
    {
        return QString("({ownerKey:'%1',runtimeGeneration:%2,streamUuid:'%3',screen:'%4'%5})")
            .arg(ownerKey).arg(runtimeGeneration).arg(streamUuid).arg(screen)
            .arg(keyUuid.isEmpty() ? QString() : QString(",keyUuid:'%1'").arg(keyUuid));
    }

    // --- Issue #144: workspace session switcher geometry oracle ------------
    // Measured against the reference at commit 61dacef78dab2f5d0be76f283bebf49fe29b84af:
    //   packages/web/src/widgets/top-bar/top-bar-workspace-session-switcher.ui.tsx
    // Tokens come from the blue-cold-dark palette (docs/NATIVE_DESKTOP_PALETTE.md).
    void sessionSwitcherTopBarAndDropdownMatchTheReference()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("preferences.ini"));
        WindowSecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        const QUrl server("https://switcher.invalid");
        struct Identity { QString user; QString project; };
        QVector<Identity> identities{
            {userId, projectId},
            {"66666666-6666-4666-8666-666666666661", "77777777-7777-4777-8777-777777777771"},
            {"66666666-6666-4666-8666-666666666662", "77777777-7777-4777-8777-777777777772"},
            {"66666666-6666-4666-8666-666666666663", "77777777-7777-4777-8777-777777777773"},
            {"66666666-6666-4666-8666-666666666664", "77777777-7777-4777-8777-777777777774"}};
        auto byToken = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        for (int index = 0; index < identities.size(); ++index) {
            SavedAccount account{SessionVault::accountId(server, identities[index].project,
                identities[index].user), server, identities[index].project, identities[index].user,
                QStringLiteral("synthetic-user-%1").arg(index), QStringLiteral("Project %1").arg(index)};
            const QString token = QStringLiteral("switch-refresh-%1").arg(index);
            byToken->insert(token, {identities[index].user, identities[index].project});
            bool stored = false;
            vault.save(account, token.toUtf8(), [&](const QString &error) { QVERIFY(error.isEmpty()); stored = true; });
            QTRY_VERIFY(stored);
        }
        auto expected = std::make_shared<QPair<QString, QString>>();
        ApiClient api;
        Session session(&api, nullptr, [byToken, expected, &api](const QByteArray &method,
            const QString &path, const QJsonValue &body, const QUrlQuery &,
            ApiClient::Callback done, bool, const QMap<QByteArray, QByteArray> &) {
            Request request{method, QUrl(path), {}, body.toObject()};
            auto response = sessionResponse(request);
            if (path.endsWith("/actions/get_token/invoke")) {
                const QString refresh = body.toObject().value("refresh_token").toString();
                *expected = byToken->value(refresh);
                auto data = response.data.toObject();
                data.insert("refresh_token", refresh);
                response.data = data;
            } else if (path.endsWith("/actions/introspect")) {
                response.data = QJsonObject{{"user_info", QJsonObject{{"uuid", expected->first}}},
                    {"project_id", expected->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                response.status = 200;
            } else if (path == "/api/core/v1/iam/projects/") {
                response.data = QJsonArray{QJsonObject{{"uuid", expected->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                response.status = 200;
            }
            const ApiResult result{response.data, response.status, {}, {}};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        const auto order = controller.savedAccounts();
        QCOMPARE(order.size(), 5);
        QStringList ids;
        for (const auto &row : order) ids.append(row.toMap().value("id").toString());
        controller.resumeSavedAccount(ids.first());
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());

        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *content = window->contentItem();
        // The app scopes appearance to the authenticated owner when the window
        // is wired, so request the dark palette (#222328/#33384d/#2c3747/
        // #3d5eff tokens) only afterwards; QML color bindings update live.
        preferences.setMode("dark");
        QTRY_VERIFY(preferences.dark());
        QVERIFY(visibleItem("topBar"));
        QVERIFY(item(content, "sessionMenuTrigger"));

        // Exactly MAX_VISIBLE_SESSIONS quick buttons (switcher :34, :167-170).
        QCOMPARE(itemCountWithPrefix(content, "sessionQuick-"), 3);
        auto *firstButton = item(content, "sessionQuick-" + ids.first());
        QVERIFY(firstButton);
        QVERIFY(!item(content, "sessionQuick-" + ids[3]));
        QCOMPARE(firstButton->width(), 40.);                 // h-10 w-10 (:125)
        QCOMPARE(firstButton->height(), 40.);
        QCOMPARE(firstButton->mapToScene(QPointF{}).x(), qreal(28));   // pl-5(20)+p-2(8)
        auto *frame = item(content, "sessionFrame-" + ids.first());
        QVERIFY(frame);
        QCOMPARE(frame->size(), QSizeF(36, 36));             // inner frame h-9 w-9 (:134)
        auto *logo = item(content, "sessionLogo-" + ids.first());
        QVERIFY(logo);
        QTRY_COMPARE(logo->property("status").toInt(), 1);  // QQuickImage::Ready
        // The live session paints the 2px border-subtle inset ring (:135).
        auto *ring = item(content, "sessionRing-" + ids.first());
        QVERIFY(ring->isVisible());
        QCOMPARE(QQmlProperty(ring, "border.width").read().toReal(), 2.);
        QCOMPARE(QQmlProperty(ring, "border.color").read().value<QColor>(), QColor("#33384d"));
        auto *otherRing = item(content, "sessionRing-" + ids[1]);
        QVERIFY2(otherRing, qPrintable(QStringLiteral("rows=%1 ids0=%2 ids1=%3")
            .arg(controller.sessionSwitcherSessions().size()).arg(ids.first(), ids[1])));
        QVERIFY(!otherRing->isVisible());
        // Overflow chip "+2" for the 3rd+ accounts (:332-336).
        auto *chip = item(content, "sessionOverflowLabel");
        QVERIFY(chip->isVisible());
        QCOMPARE(chip->property("text").toString(), QString("+2"));

        // Unread projection ladder drives the blue 10px dot (unread lib).
        const QString allFolders{"00000000-0000-0000-0000-000000000000"};
        controller.publishSessionProjection(ids[1], QVariantMap{
            {"unreadByFolderId", QVariantMap{{allFolders, 37}}}});
        // Publishing rebuilds the switcher delegates, so poll for a fresh
        // item instead of dereferencing a possibly replaced pointer.
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *d = item(content, "sessionDot-" + ids[1]);
            return d && d->isVisible(); })(), 5000);
        auto *dot = item(content, "sessionDot-" + ids[1]);
        QVERIFY(dot);
        QCOMPARE(dot->size(), QSizeF(10, 10));               // h-2.5 w-2.5 (:145)
        QCOMPARE(dot->property("color").value<QColor>(), QColor("#3d5eff"));
        QCOMPARE(QQmlProperty(dot, "border.width").read().toReal(), 1.);
        QCOMPARE(QQmlProperty(dot, "border.color").read().value<QColor>(), QColor("#141517"));

        // Dropdown: every remembered account (:173-184), current row bg (:188).
        click(window, item(content, "sessionMenuTrigger"));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *m = item(content, "sessionSwitcherPopup");
            return m && m->isVisible(); })(), 5000);
        QCOMPARE(itemCountWithPrefix(content, "sessionRow-"), 5);
        QCOMPARE(item(content, "sessionRow-" + ids.first())->property("color").value<QColor>(),
            QColor("#2c3747"));
        // Badges cap at the 1000 display limit (count-format.lib.ts:1).
        controller.publishSessionProjection(ids[4], QVariantMap{
            {"unreadByFolderId", QVariantMap{{allFolders, 1500}}}});
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *b = item(content, "sessionRowBadgeLabel-" + ids[4]);
            return b && b->isVisible(); })(), 5000);
        auto *badge = item(content, "sessionRowBadgeLabel-" + ids[4]);
        QVERIFY(badge);
        QCOMPARE(badge->property("text").toString(), QString("1000+"));
        auto *logoutButton = item(content, "sessionLogout-" + ids.first());
        QVERIFY(logoutButton);
        QCOMPARE(logoutButton->size(), QSizeF(24, 24));      // icon-btn-ghost p-1 (:270)
        const QString firstTitle = controller.sessionSwitcherSessions().first()
            .toMap().value("title").toString();
        // "Logout from organization" asks, then the next remembered inbox wins.
        click(window, logoutButton);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *c = item(content, "sessionLogoutConfirm");
            return c && c->isVisible(); })(), 5000);
        QCOMPARE(item(content, "sessionLogoutConfirmText")->property("text").toString(),
            QStringLiteral("Log out from ") + firstTitle + QStringLiteral("?"));
        click(window, item(content, "sessionLogoutConfirmButton"));
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.savedAccounts().size(), 4);
        const auto nextRows = controller.sessionSwitcherSessions();
        QCOMPARE(nextRows.first().toMap().value("id").toString(), ids[1]);
        QTRY_COMPARE(nextRows.first().toMap().value("isCurrent"), QVariant(true));
        QVERIFY(!backend.values.contains(ids.first()));
        QVERIFY(backend.values.contains(ids[1]));
        // "Add server" opens sign-in with the remembered sessions preserved.
        click(window, item(content, "sessionMenuTrigger"));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *a = item(content, "sessionAddServer");
            return a && a->isVisible(); })(), 5000);
        click(window, item(content, "sessionAddServer"));
        QTRY_VERIFY(!controller.authenticated());
        QVERIFY(item(content, "serverInput"));
        QCOMPARE(controller.savedAccounts().size(), 4);
        QTRY_VERIFY(item(content, "resume-" + ids[2]));
    }

    void sessionSwitcherPopupKeepsActionsInsideTheWindow()
    {
        // exo-review #170: with enough remembered accounts the dropdown must
        // stay bounded by the window and scroll, so "Add server" remains
        // reachable instead of being pushed outside the visible window.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("preferences.ini"));
        WindowSecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        const QUrl server("https://switcher.invalid");
        struct Identity { QString user; QString project; };
        QVector<Identity> identities;
        for (int index = 0; index < 16; ++index)
            identities.append({QStringLiteral("66666666-6666-4666-8666-6666666666%1")
                    .arg(index, 2, 10, QLatin1Char('0')),
                QStringLiteral("77777777-7777-4777-8777-7777777777%1")
                    .arg(index, 2, 10, QLatin1Char('0'))});
        auto byToken = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        for (int index = 0; index < identities.size(); ++index) {
            SavedAccount account{SessionVault::accountId(server, identities[index].project,
                identities[index].user), server, identities[index].project, identities[index].user,
                QStringLiteral("synthetic-user-%1").arg(index), QStringLiteral("Project %1").arg(index)};
            const QString token = QStringLiteral("switch-refresh-%1").arg(index);
            byToken->insert(token, {identities[index].user, identities[index].project});
            bool stored = false;
            vault.save(account, token.toUtf8(), [&](const QString &error) { QVERIFY(error.isEmpty()); stored = true; });
            QTRY_VERIFY(stored);
        }
        auto expected = std::make_shared<QPair<QString, QString>>();
        ApiClient api;
        Session session(&api, nullptr, [byToken, expected, &api](const QByteArray &method,
            const QString &path, const QJsonValue &body, const QUrlQuery &,
            ApiClient::Callback done, bool, const QMap<QByteArray, QByteArray> &) {
            Request request{method, QUrl(path), {}, body.toObject()};
            auto response = sessionResponse(request);
            if (path.endsWith("/actions/get_token/invoke")) {
                const QString refresh = body.toObject().value("refresh_token").toString();
                *expected = byToken->value(refresh);
                auto data = response.data.toObject();
                data.insert("refresh_token", refresh);
                response.data = data;
            } else if (path.endsWith("/actions/introspect")) {
                response.data = QJsonObject{{"user_info", QJsonObject{{"uuid", expected->first}}},
                    {"project_id", expected->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                response.status = 200;
            } else if (path == "/api/core/v1/iam/projects/") {
                response.data = QJsonArray{QJsonObject{{"uuid", expected->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                response.status = 200;
            }
            const ApiResult result{response.data, response.status, {}, {}};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        const auto order = controller.savedAccounts();
        QCOMPARE(order.size(), 16);
        controller.resumeSavedAccount(order.first().toMap().value("id").toString());
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *content = window->contentItem();
        // Sixteen account rows (~56 px each) exceed any legal window height
        // (Main.qml starts at 900 and cannot grow the menu beyond the window),
        // so the menu has to bound itself to the window and scroll.
        QVERIFY(window->height() < 16 * 56 + 45);
        QVERIFY(visibleItem("topBar"));
        click(window, item(content, "sessionMenuTrigger"));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *m = item(content, "sessionSwitcherPopup");
            return m && m->isVisible(); })(), 5000);
        QCOMPARE(itemCountWithPrefix(content, "sessionRow-"), 16);
        auto *scroll = item(content, "sessionSwitcherScroll");
        QVERIFY2(scroll, "menu contents must live in a scrollable viewport (sessionSwitcherScroll)");
        const auto sceneTop = [](QQuickItem *it) { return it->mapToScene(QPointF{}).y(); };
        const auto sceneBottom = [&](QQuickItem *it) {
            return it->mapToScene(QPointF(0, it->height())).y();
        };
        QTRY_VERIFY_WITH_TIMEOUT(sceneTop(scroll) >= 0.
            && sceneBottom(scroll) <= window->height() + 0.5, 5000);
        // The content is taller than the bounded viewport, and scrolling the
        // viewport moves the content (review #170: the menu must scroll).
        QVERIFY2(scroll->property("contentHeight").toReal() > scroll->height() + 0.5,
            "menu content taller than the window must be scrollable");
        scroll->setProperty("contentY", 200);
        auto *addServer = item(content, "sessionAddServer");
        QVERIFY(addServer);
        // After scrolling to the bottom the Add server action sits inside the
        // visible menu viewport (and therefore inside the window).
        QTRY_VERIFY_WITH_TIMEOUT(sceneTop(addServer) >= sceneTop(scroll) - 0.5
            && sceneBottom(addServer) <= sceneBottom(scroll) + 0.5, 5000);
        QVERIFY2(sceneBottom(addServer) <= window->height() + 0.5,
            "Add server must stay inside the window bounds");
    }

    void sessionSwitcherRowsActivateFromKeyboard()
    {
        // exo-review #170: account rows are selectable controls; keyboard
        // users must be able to reach and activate them (Enter and Space).
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("preferences.ini"));
        WindowSecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        const QUrl server("https://switcher.invalid");
        struct Identity { QString user; QString project; };
        QVector<Identity> identities{
            {userId, projectId},
            {"66666666-6666-4666-8666-666666666661", "77777777-7777-4777-8777-777777777771"},
            {"66666666-6666-4666-8666-666666666662", "77777777-7777-4777-8777-777777777772"},
            {"66666666-6666-4666-8666-666666666663", "77777777-7777-4777-8777-777777777773"},
            {"66666666-6666-4666-8666-666666666664", "77777777-7777-4777-8777-777777777774"}};
        auto byToken = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        for (int index = 0; index < identities.size(); ++index) {
            SavedAccount account{SessionVault::accountId(server, identities[index].project,
                identities[index].user), server, identities[index].project, identities[index].user,
                QStringLiteral("synthetic-user-%1").arg(index), QStringLiteral("Project %1").arg(index)};
            const QString token = QStringLiteral("switch-refresh-%1").arg(index);
            byToken->insert(token, {identities[index].user, identities[index].project});
            bool stored = false;
            vault.save(account, token.toUtf8(), [&](const QString &error) { QVERIFY(error.isEmpty()); stored = true; });
            QTRY_VERIFY(stored);
        }
        auto expected = std::make_shared<QPair<QString, QString>>();
        ApiClient api;
        Session session(&api, nullptr, [byToken, expected, &api](const QByteArray &method,
            const QString &path, const QJsonValue &body, const QUrlQuery &,
            ApiClient::Callback done, bool, const QMap<QByteArray, QByteArray> &) {
            Request request{method, QUrl(path), {}, body.toObject()};
            auto response = sessionResponse(request);
            if (path.endsWith("/actions/get_token/invoke")) {
                const QString refresh = body.toObject().value("refresh_token").toString();
                *expected = byToken->value(refresh);
                auto data = response.data.toObject();
                data.insert("refresh_token", refresh);
                response.data = data;
            } else if (path.endsWith("/actions/introspect")) {
                response.data = QJsonObject{{"user_info", QJsonObject{{"uuid", expected->first}}},
                    {"project_id", expected->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                response.status = 200;
            } else if (path == "/api/core/v1/iam/projects/") {
                response.data = QJsonArray{QJsonObject{{"uuid", expected->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                response.status = 200;
            }
            const ApiResult result{response.data, response.status, {}, {}};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        const auto order = controller.savedAccounts();
        QCOMPARE(order.size(), 5);
        QStringList ids;
        for (const auto &row : order) ids.append(row.toMap().value("id").toString());
        controller.resumeSavedAccount(ids.first());
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *content = window->contentItem();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(visibleItem("topBar"));
        click(window, item(content, "sessionMenuTrigger"));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *m = item(content, "sessionSwitcherPopup");
            return m && m->isVisible(); })(), 5000);
        // The row past the visible quick buttons is keyboard reachable.
        // The switcher's delegates are recreated whenever the controller
        // re-emits its rows, so grab focus on whichever live instance exists
        // and activate it immediately.
        const auto focusRowAndPress = [&](const QString &rowName, Qt::Key key) {
            QTRY_VERIFY_WITH_TIMEOUT(([&] {
                auto *r = item(content, rowName);
                if (!r) return false;
                r->setFocus(true);
                return r->hasActiveFocus();
            })(), 8000);
            QTest::keyClick(window, key);
        };
        auto *row = item(content, "sessionRow-" + ids[4]);
        QVERIFY(row);
        QVERIFY2(row->property("activeFocusOnTab").toBool(),
            "account rows must participate in the tab chain");
        focusRowAndPress("sessionRow-" + ids[4], Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ const auto rows = controller.sessionSwitcherSessions();
            return !rows.isEmpty() && rows.first().toMap().value("id").toString() == ids[4]
                && rows.first().toMap().value("isCurrent").toBool(); })(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *m = item(content, "sessionSwitcherPopup");
            return !m || !m->isVisible(); })(), 5000);
        // Space activates too (button semantics).
        click(window, item(content, "sessionMenuTrigger"));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *m = item(content, "sessionSwitcherPopup");
            return m && m->isVisible(); })(), 5000);
        auto *otherRow = item(content, "sessionRow-" + ids[3]);
        QVERIFY(otherRow);
        focusRowAndPress("sessionRow-" + ids[3], Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ const auto rows = controller.sessionSwitcherSessions();
            return !rows.isEmpty() && rows.first().toMap().value("id").toString() == ids[3]
                && rows.first().toMap().value("isCurrent").toBool(); })(), 10000);
    }

    void sessionSwitcherFocusedRowScrollsIntoView()
    {
        // exo-review #170 round 2: keyboard focus must never land on a row
        // clipped outside the menu viewport; focusing such a row scrolls it
        // into view instead of leaving the focus ring offscreen.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("preferences.ini"));
        WindowSecrets backend;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        const QUrl server("https://switcher.invalid");
        struct Identity { QString user; QString project; };
        QVector<Identity> identities;
        for (int index = 0; index < 16; ++index)
            identities.append({QStringLiteral("66666666-6666-4666-8666-6666666666%1")
                    .arg(index, 2, 10, QLatin1Char('0')),
                QStringLiteral("77777777-7777-4777-8777-7777777777%1")
                    .arg(index, 2, 10, QLatin1Char('0'))});
        auto byToken = std::make_shared<QHash<QString, QPair<QString, QString>>>();
        for (int index = 0; index < identities.size(); ++index) {
            SavedAccount account{SessionVault::accountId(server, identities[index].project,
                identities[index].user), server, identities[index].project, identities[index].user,
                QStringLiteral("synthetic-user-%1").arg(index), QStringLiteral("Project %1").arg(index)};
            const QString token = QStringLiteral("switch-refresh-%1").arg(index);
            byToken->insert(token, {identities[index].user, identities[index].project});
            bool stored = false;
            vault.save(account, token.toUtf8(), [&](const QString &error) { QVERIFY(error.isEmpty()); stored = true; });
            QTRY_VERIFY(stored);
        }
        auto expected = std::make_shared<QPair<QString, QString>>();
        ApiClient api;
        Session session(&api, nullptr, [byToken, expected, &api](const QByteArray &method,
            const QString &path, const QJsonValue &body, const QUrlQuery &,
            ApiClient::Callback done, bool, const QMap<QByteArray, QByteArray> &) {
            Request request{method, QUrl(path), {}, body.toObject()};
            auto response = sessionResponse(request);
            if (path.endsWith("/actions/get_token/invoke")) {
                const QString refresh = body.toObject().value("refresh_token").toString();
                *expected = byToken->value(refresh);
                auto data = response.data.toObject();
                data.insert("refresh_token", refresh);
                response.data = data;
            } else if (path.endsWith("/actions/introspect")) {
                response.data = QJsonObject{{"user_info", QJsonObject{{"uuid", expected->first}}},
                    {"project_id", expected->second}, {"otp_verified", true},
                    {"permissions", QJsonArray{}}};
                response.status = 200;
            } else if (path == "/api/core/v1/iam/projects/") {
                response.data = QJsonArray{QJsonObject{{"uuid", expected->second},
                    {"name", "Switcher project"}, {"status", "ACTIVE"}}};
                response.status = 200;
            }
            const ApiResult result{response.data, response.status, {}, {}};
            QTimer::singleShot(0, &api, [done, result] { done(result); });
        });
        session.setVault(&vault);
        NativeController controller(&api, &session);
        const auto order = controller.savedAccounts();
        QCOMPARE(order.size(), 16);
        QStringList ids;
        for (const auto &row : order) ids.append(row.toMap().value("id").toString());
        controller.resumeSavedAccount(ids.first());
        QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(!controller.busy());
        QQmlApplicationEngine engine;
        loadNativeUi(engine, &controller, &preferences);
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *content = window->contentItem();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(visibleItem("topBar"));
        click(window, item(content, "sessionMenuTrigger"));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *m = item(content, "sessionSwitcherPopup");
            return m && m->isVisible(); })(), 5000);
        auto *scroll = item(content, "sessionSwitcherScroll");
        QVERIFY(scroll);
        const auto rowTop = [&](QQuickItem *r) { return r->mapToItem(scroll, QPointF{}).y(); };
        const auto rowBottom = [&](QQuickItem *r) {
            return r->mapToItem(scroll, QPointF(0, r->height())).y();
        };
        const auto rowInViewport = [&](QQuickItem *r) {
            return rowTop(r) >= -0.5 && rowBottom(r) <= scroll->height() + 0.5;
        };
        // The last row sits below the clipped viewport while the menu is at
        // its natural scroll position.
        const QString lastRowName = QStringLiteral("sessionRow-") + ids.last();
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *r = item(content, lastRowName);
            return r && rowTop(r) > scroll->height(); })(), 5000);
        // Keyboard focus enters that offscreen row (Tab reaches it: the rows
        // are tab-chain controls, so setFocus models the same focus move).
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            auto *r = item(content, lastRowName);
            if (!r) return false;
            r->setFocus(true);
            return r->hasActiveFocus();
        })(), 8000);
        auto *focused = item(content, lastRowName);
        QVERIFY(focused);
        QVERIFY2(focused->hasActiveFocus(), "the offscreen row must hold active focus");
        QVERIFY2(!rowInViewport(focused) || scroll->property("contentY").toReal() > 0.5,
            qPrintable(QStringLiteral("viewport must move for a focused row (contentY=%1, rowTop=%2, viewport=%3)")
                .arg(scroll->property("contentY").toReal()).arg(rowTop(focused)).arg(scroll->height())));
        // The focused row is now inside the clipped viewport.
        QTRY_VERIFY_WITH_TIMEOUT(([&]{ auto *r = item(content, lastRowName);
            return r && rowInViewport(r); })(), 5000);
        // The viewport actually scrolled.
        QVERIFY(scroll->property("contentY").toReal() > 0.5);
    }

    // ---- Issue #123: composer mention popup, insert, URN send and restart frecency ----
    static bool mentionPopupVisible(QQuickWindow *window)
    {
        auto *popup = window->findChild<QObject *>("composerMentionPopup");
        return popup && popup->property("visible").toBool();
    }
    // Popup content is parented visually (setParentItem), so C++ findChild never
    // reaches delegate items; walk the visual tree instead.
    static QQuickItem *mentionVisualChild(QQuickItem *root, const QString &name)
    {
        const auto children = root->childItems();
        for (auto *child : children)
        {
            if (child->objectName() == name) return child;
            if (auto *nested = mentionVisualChild(child, name)) return nested;
        }
        return nullptr;
    }
    static QStringList mentionRowTexts(QQuickWindow *window)
    {
        QStringList rows;
        auto *content = window->contentItem();
        for (int index = 0; index < 8; ++index)
        {
            auto *name = mentionVisualChild(content, QString("composerMentionName%1").arg(index));
            if (!name || !name->property("visible").toBool()) break;
            rows.append(name->property("text").toString());
        }
        return rows;
    }

    private slots:
    void composerMentionPopupInsertsUrnsAndKeepsFrecencyAcrossRestart()
    {
        const QString mentionBob = QStringLiteral("52000000-0000-4000-8000-00000000000b");
        const QString mentionBix = QStringLiteral("52000000-0000-4000-8000-0000000000c0");
        WindowChannelFixture fixture; fixture.sourceScene = true;
        fixture.users = {QJsonObject{{"uuid", userId}, {"username", "cassi"}},
            QJsonObject{{"uuid", channelPeer}, {"username", "alice"}, {"first_name", "Alice"},
                        {"email", "alice@example.invalid"}},
            QJsonObject{{"uuid", mentionBob}, {"username", "bob"}, {"first_name", "Bob"}, {"last_name", "Ray"}},
            QJsonObject{{"uuid", mentionBix}, {"username", "bix"}, {"first_name", "Bix"}, {"last_name", "Ray"}}};
        int posts = 0, confirmations = 0;
        QJsonObject posted, saved;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST" && path == "/api/workspace/v1/messenger/messages/") {
                ++posts; posted = request.body;
                const auto content = request.body.value("payload").toObject().value("content").toString();
                saved = message(content, QStringLiteral("8c000000-0000-4000-8000-000000000001"));
                saved["user_uuid"] = userId; saved["updated_at"] = saved.value("created_at");
                return {saved, 201};
            }
            if (request.method == "GET" && !saved.isEmpty()
                && path == "/api/workspace/v1/messenger/messages/" + saved.value("uuid").toString()) {
                ++confirmations; return {saved};
            }
            return fallback(request);
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("composer-mention.ini"));
        ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
        QQmlApplicationEngine engine; loadNativeUi(engine, &controller, &preferences);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)); QVERIFY(window);
        auto close = qScopeGuard([&engine, guard = QPointer<QQuickWindow>(window)] {
            if (guard && !closeIncubatedTestWindow(engine, guard.data()))
                QTest::qFail("Composer mention teardown", __FILE__, __LINE__);
        });
        QVERIFY(QTest::qWaitForWindowExposed(window)); window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(startComposerEmojiFixture(fixture, controller, session));
        auto *composer = item(window->contentItem(), "messageComposer");
        QVERIFY(composer && composer->isEnabled());
        // An @ inside a word never opens the popup (reference: "does not open
        // mention popup when @ is inside a word or email").
        click(window, composer);
        for (const auto key : QByteArray("hello@a")) QTest::keyClick(window, key);
        QTRY_COMPARE(controller.draft(), QString("hello@a"));
        QVERIFY(!mentionPopupVisible(window));
        // A standalone @ opens the popup; the query refines it; Escape closes it.
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_At); QTest::keyClick(window, Qt::Key_A);
        QTRY_VERIFY(mentionPopupVisible(window));
        // "a": prefix tier (Alice) first, then substring-tier matches on the
        // "Ray" surname (reference mention-suggest tiers).
        QTRY_COMPARE(mentionRowTexts(window),
            (QStringList{QString("Alice"), QString("Bix Ray"), QString("Bob Ray")}));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!mentionPopupVisible(window));
        // Two equal "B" matches, tied on every context score; the reference tie
        // order is locale-aware alphabetical, so "Bix Ray" sits above "Bob Ray".
        // ArrowDown reaches the second row and Enter inserts "@Bob Ray " with
        // the caret after the trailing space without sending.
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_At);
        QTest::keyClick(window, Qt::Key_B);
        QTRY_VERIFY(mentionPopupVisible(window));
        QTRY_COMPARE(mentionRowTexts(window), (QStringList{QString("Bix Ray"), QString("Bob Ray")}));
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(composer->property("text").toString(), QString("@Bob Ray "));
        QCOMPARE(composer->property("cursorPosition").toInt(), 9);
        QVERIFY(!mentionPopupVisible(window));
        QCOMPARE(posts, 0);
        // A no-match query shows the no-results row (search.noResults). The
        // inserted mention already ends with a trailing space, so typing
        // continues directly after it.
        QTest::keyClick(window, Qt::Key_End);
        for (const auto key : QByteArray("hi @zz"))
            if (key == ' ') QTest::keyClick(window, Qt::Key_Space); else QTest::keyClick(window, key);
        QTRY_VERIFY(mentionPopupVisible(window));
        QVERIFY(window->findChild<QObject *>("composerMentionNoResults"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!mentionPopupVisible(window));
        // Enter sends: the visible mention becomes the canonical URN and the
        // literal "@zz" survives untouched.
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(posts, 1);
        QTRY_COMPARE(confirmations, 1);
        QCOMPARE(posted.value("payload").toObject().value("content").toString(),
            QStringLiteral("[Bob Ray](urn:user:%1) hi @zz").arg(mentionBob));
        // The pick persists across a fresh preferences instance on the same store
        // and keeps the restart ranking order (Bob above Bix for query "b").
        NativePreferences reopened(directory.filePath("composer-mention.ini"));
        // Picks are scoped to the loopback-safe creation owner (drafts scope).
        QVERIFY(reopened.mentionFrecencyFor(controller.creationOwnerKey()).value(mentionBob).toDouble() >= 0.9);
        QVERIFY(!reopened.mentionFrecencyFor(controller.creationOwnerKey()).contains(mentionBix));
        controller.setPreferences(&reopened);
        QCOMPARE(controller.mentionSuggestions(QStringLiteral("b")).first().toMap()
            .value("displayName").toString(), QString("Bob Ray"));
        QTRY_VERIFY(api.findChildren<QNetworkReply *>().isEmpty());
        const bool closed = closeIncubatedTestWindow(engine, window);
        if (closed) close.dismiss(); QVERIFY(closed);
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
            // The Source edit mode is the composer itself: a notice bar above the input
            // (6+20+6 px plus the 1 px rule) and the message body pre-filled.
            auto *editNotice = item(content, "editMessageNotice");
            QVERIFY(editNotice);
            QTRY_VERIFY(editNotice->property("visible").toBool());
            QCOMPARE(editNotice->height(), 33.0);
            QCOMPARE(item(content, "editMessageNoticeText")->property("text").toString(), QString("Edit message"));
            // The composer is pre-filled with the body the server last confirmed.
            const auto confirmedBody = controller.messages().first().toMap().value("content").toString();
            QVERIFY(!confirmedBody.isEmpty());
            QTRY_COMPARE(composer->property("text").toString(), confirmedBody);
            // The send control is reused as the save control while the session is open.
            QVERIFY(window->findChild<QObject *>("saveEditButton"));
            // A blank buffer cannot be saved, exactly like the disabled send control in Source.
            composer->setProperty("text", " ");
            QTRY_VERIFY(!controller.canSaveEdit());
            composer->setProperty("text", "Uncommitted edit during connection recovery");
            QTRY_VERIFY(controller.canSaveEdit());
            controller.refresh();
            QVERIFY(controller.busy());
            QVERIFY(editNotice->property("visible").toBool());
            QTRY_VERIFY(!controller.busy());
            QVERIFY(editNotice->property("visible").toBool());
            QCOMPARE(composer->property("text").toString(), QString("Uncommitted edit during connection recovery"));
            QCOMPARE(controller.editingMessageId(), messageId);
            QCOMPARE(controller.draft(), QString("Uncommitted edit during connection recovery"));
            // Escape leaves the mode and the draft written before the edit is back.
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!editNotice->property("visible").toBool());
            QVERIFY(controller.editingMessageId().isEmpty());
            QCOMPARE(controller.draft(), QString("Draft while saved history is being checked"));

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
    if (qEnvironmentVariableIsSet("CASSI_NATIVE_DRAFT_WORKLOAD")) {
        NativeDraftWorkload workload;
        return QTest::qExec(&workload, argc, argv);
    }
    if (qEnvironmentVariableIsSet("CASSI_NATIVE_CACHE_CHILD_PHASE")) {
        NativeCacheWindowWorker worker;
        return QTest::qExec(&worker, argc, argv);
    }
    NativeWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "native-window-test.moc"
