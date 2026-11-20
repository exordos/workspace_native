#include "live-acceptance-trace.h"
#include "api-client.h"
#include "native-cache-service.h"
#include "native-controller.h"
#include "realtime-client.h"

#include <QEvent>
#include <QHash>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QUrlQuery>
#include <QWheelEvent>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace workspace {
namespace {
qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

QQuickItem *findVisual(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name) return parent;
    for (auto *child : parent->childItems())
        if (auto *item = findVisual(child, name)) return item;
    return nullptr;
}

class Trace final : public QObject {
public:
    Trace(int fd, QQuickWindow *window, NativeController &controller, NativeCacheService &cache)
        : QObject(window), pipe(fd), window(window), controller(controller), cache(cache) {}

    void writeEvent(const char *kind, QJsonObject values = {})
    {
        values.insert("event", QLatin1String(kind));
        values.insert("ms", nowMs());
        const auto line = QJsonDocument(values).toJson(QJsonDocument::Compact) + '\n';
        if (::write(pipe, line.constData(), line.size()) != line.size())
            ++dropped;
    }

    void sample()
    {
        if (!list) list = findVisual(window->contentItem(), QStringLiteral("messageList"));
        if (!editor) editor = findVisual(window->contentItem(), QStringLiteral("messageComposer"));
        QJsonObject values{{"active", window->isActive()}, {"authenticated", controller.authenticated()},
                           {"ready", controller.messageHistoryReady()}, {"frames", qint64(frames.load())},
                           {"dropped", qint64(dropped.load())}};
        const auto selection = controller.selectedTopic();
        values.insert("view", !controller.authenticated() ? "none" : selection == topicA && !topicA.isEmpty()
            ? "a" : selection == topicB && !topicB.isEmpty() ? "b" : selection.isEmpty() ? "aggregate" : "other");
        if (list && list->isVisible()) {
            values.insert("count", list->property("count").toInt());
            values.insert("offset", list->property("contentY").toDouble() - list->property("originY").toDouble());
            values.insert("intent", list->property("userScrollGeneration").toInt());
            const auto pane = list->mapRectToScene(list->boundingRect()).toAlignedRect();
            values.insert("paneX", pane.x());
            values.insert("paneY", pane.y());
            values.insert("width", pane.width());
            values.insert("height", pane.height());
            QQuickItem *row = nullptr;
            const qreal y = list->property("contentY").toDouble() + list->height() / 2;
            QMetaObject::invokeMethod(list, "itemAt", Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(qreal, qreal(20)), Q_ARG(qreal, y));
            if (row) {
                const auto entry = row->property("entry");
                const auto map = entry.canConvert<QJSValue>() ? entry.value<QJSValue>().toVariant().toMap() : entry.toMap();
                const auto key = map.value("id").toString();
                if (!key.isEmpty()) {
                    // Stable only within this process. Identifiers never enter the trace.
                    if (!anchors.contains(key)) anchors.insert(key, anchors.size() + 1);
                    values.insert("anchor", anchors.value(key));
                    values.insert("anchorY", row->mapToItem(list, QPointF{}).y());
                }
            }
        }
        if (editor && editor->isVisible()) {
            values.insert("editorFocus", editor->hasActiveFocus());
            values.insert("draftLength", editor->property("length").toInt());
        }
        const auto stats = cache.stats();
        values.insert("hits", qint64(stats.hits));
        values.insert("misses", qint64(stats.misses));
        writeEvent("sample", values);
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == window && event->type() == QEvent::MouseButtonRelease
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
            writeEvent("click");
        if (object == window && event->type() == QEvent::Wheel && list && list->isVisible()) {
            const auto *wheel = static_cast<QWheelEvent *>(event);
            const auto point = list->mapFromScene(wheel->position());
            if (list->contains(point))
                writeEvent("wheel", {{"angle", wheel->angleDelta().y()}, {"pixel", wheel->pixelDelta().y()},
                                     {"phase", int(wheel->phase())}});
        }
        return false;
    }

    int pipe;
    QQuickWindow *window;
    NativeController &controller;
    NativeCacheService &cache;
    const QString topicA = qEnvironmentVariable("WORKSPACE_NATIVE_ACCEPTANCE_TOPIC_A");
    const QString topicB = qEnvironmentVariable("WORKSPACE_NATIVE_ACCEPTANCE_TOPIC_B");
    QHash<QString, int> anchors;
    QPointer<QQuickItem> list, editor;
    std::atomic<int> frames{0}, dropped{0};
};
}

void configureLiveAcceptanceNetwork()
{
    bool ok = false;
    const int fd = qEnvironmentVariableIntValue("WORKSPACE_NATIVE_ACCEPTANCE_FD", &ok);
    struct stat info{};
    if (!ok || fd < 3 || fstat(fd, &info) || !S_ISFIFO(info.st_mode)) return;
    const int port = qEnvironmentVariableIntValue("WORKSPACE_NATIVE_ACCEPTANCE_PROXY_PORT", &ok);
    if (ok && port > 0 && port <= 65535)
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", port));
}

void observeLiveAcceptance(QQuickWindow *window, ApiClient &api, NativeController &controller,
                           NativeCacheService &cache, RealtimeClient &realtime)
{
    bool ok = false;
    const int fd = qEnvironmentVariableIntValue("WORKSPACE_NATIVE_ACCEPTANCE_FD", &ok);
    struct stat info{};
    if (!window || !ok || fd < 3 || fstat(fd, &info) || !S_ISFIFO(info.st_mode)) return;
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    auto *trace = new Trace(fd, window, controller, cache);
    trace->writeEvent("runtime", {{"qt", QString::fromLatin1(qVersion())}, {"scale", window->devicePixelRatio()},
        {"graphicsApi", int(window->rendererInterface()->graphicsApi())}});
    window->installEventFilter(trace);
    QObject::connect(window, &QQuickWindow::frameSwapped, trace, [trace] {
        ++trace->frames;
        trace->writeEvent("frame");
    }, Qt::DirectConnection);
    auto *timer = new QTimer(trace);
    timer->setTimerType(Qt::PreciseTimer);
    QObject::connect(timer, &QTimer::timeout, trace, &Trace::sample);
    timer->start(25);
    QObject::connect(&api, &ApiClient::requestDispatched, trace, [trace](QNetworkReply *reply) {
        const auto url = reply->request().url();
        const auto path = url.path();
        QString category = QStringLiteral("other");
        if (path.endsWith("/messages/")) category = QStringLiteral("history");
        else if (path.contains("/files/")) category = QStringLiteral("image");
        else if (path.contains("/iam/")) category = QStringLiteral("auth");
        else if (path.contains("/events")) category = QStringLiteral("events");
        const auto query = QUrlQuery(url);
        const bool older = query.hasQueryItem("page_marker") || query.hasQueryItem("older_than");
        const auto start = nowMs();
        trace->writeEvent("request", {{"category", category}, {"older", older}});
        QObject::connect(reply, &QNetworkReply::finished, trace, [trace, reply, start, category, older] {
            trace->writeEvent("response", {{"category", category}, {"older", older}, {"elapsed", nowMs()-start},
                {"status", reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()}});
        });
    });
    const auto peerMessage = qgetenv("WORKSPACE_NATIVE_ACCEPTANCE_PEER_MESSAGE");
    QObject::connect(&realtime, &RealtimeClient::eventReceived, trace, [trace, peerMessage](const QJsonObject &event) {
        const bool peer = !peerMessage.isEmpty() && QJsonDocument(event).toJson(QJsonDocument::Compact).contains(peerMessage);
        trace->writeEvent("realtime", {{"peer", peer}});
    });
}
}
